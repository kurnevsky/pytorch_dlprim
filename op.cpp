#include <torch/torch.h>
#include <ATen/ATen.h>
#include "CLTensor.h"

#include <dlprim/core/ip.hpp>
#include <dlprim/core/conv.hpp>
#include <dlprim/core/pool.hpp>
#include <dlprim/core/activation.hpp>

#include <iostream>

namespace ptdlprim {

using namespace torch;

using c10::Device;
using c10::DeviceType;

class OCLDevImpl : public c10::impl::DeviceGuardImplInterface {
public:
    OCLDevImpl() :
        dt_(DeviceType::OPENCL,0),
        s_(c10::Stream::UNSAFE,dt_,0)
    {
    } 
    virtual DeviceType type() const { return c10::DeviceType::OPENCL; }
    virtual Device exchangeDevice(Device d) const {
        Device prev = dt_;
        dt_ = d;
        return prev;
    }
    virtual Device getDevice() const { return dt_; }
    virtual void setDevice(Device d) const { dt_ = d; }
    virtual void uncheckedSetDevice(Device d) const noexcept  { dt_ = d; }
    virtual c10::Stream getStream(Device d) const noexcept { 
        return c10::Stream(c10::Stream::UNSAFE,d,0);
    }
    virtual c10::Stream getDefaultStream(Device d) const { return getStream(d); }
    virtual c10::Stream exchangeStream(Stream) const noexcept { return getStream(dt_); }
    virtual DeviceIndex deviceCount() const noexcept { 
        try {
            return CLContextManager::count();
        }
        catch(...) {
            return 0;
        }
    }
    virtual bool queryStream(const Stream& /*stream*/) const {
        return false;
    }
    virtual void synchronizeStream(const Stream& stream) const {
        auto device = stream.device();
        CLContextManager::getCommandQueue(device.index()).finish();
    }
private:
    mutable Device dt_;
    mutable Stream s_;
} ocl_impl_instance;

// register backend
c10::impl::DeviceGuardImplRegistrar ocl_impl_reg(c10::DeviceType::OPENCL,&ocl_impl_instance);

    using torch::Tensor;
    
    torch::Tensor allocate_empty(torch::IntArrayRef size, c10::optional<ScalarType> dtype, c10::optional<Layout> /*layout*/, c10::optional<Device> device, c10::optional<bool> /*pin_memory*/, c10::optional<MemoryFormat> /*memory_format*/)
    {
        c10::ScalarType st = dtype ? *dtype : c10::kFloat; 
        c10::Device dev = device ? *device : Device(c10::DeviceType::OPENCL,0);
        return ptdlprim::new_ocl_tensor(size,dev,st);
    }

    /// "aten::empty_strided"
    Tensor empty_strided(torch::IntArrayRef size, torch::IntArrayRef /*stride*/, c10::optional<ScalarType> dtype, c10::optional<Layout> layout, c10::optional<Device> device, c10::optional<bool> pin_memory) 
    {
        return allocate_empty(size,dtype,layout,device,pin_memory,c10::nullopt);
    }


    torch::Tensor _reshape_alias(const Tensor & self, c10::IntArrayRef size, c10::IntArrayRef stride)
    {
        torch::Tensor data(self);
        data.getIntrusivePtr()->set_sizes_and_strides(size,stride);
        return data;
    }
    Tensor &fill_(Tensor &self, const c10::Scalar &value)
    {
        dlprim::Tensor t(todp(self));
        auto q = getExecutionContext(self);
        dlprim::core::fill_tensor(t,value.to<double>(),q);
        sync_if_needed(self.device());
        return self;
    }
    Tensor &zero_(Tensor &self)
    {
        dlprim::Tensor t(todp(self));
        dlprim::core::fill_tensor(t,0.0,getExecutionContext(self));
        return self;
    }


    Tensor _copy_from(const Tensor & self, const Tensor & dst, bool non_blocking)
    {
        if(dst.device().type() == c10::DeviceType::CPU && self.device().type() == c10::DeviceType::OPENCL) {
            dlprim::Tensor t(todp(self));
            auto ec = getExecutionContext(self);
            void *ptr = dst.data_ptr();
            t.to_host(ec,ptr);
        }
        else if(self.device().type() == c10::DeviceType::CPU && dst.device().type() == c10::DeviceType::OPENCL) {
            dlprim::Tensor t(todp(dst));
            auto ec = getExecutionContext(dst);
            void *ptr = self.data_ptr();
            t.to_device(ec,ptr);
        }
        else {
            throw std::runtime_error("OpenCL supports copy to CPU backend only");
        }
        return self;
    }
    Tensor convolution_overrideable(const Tensor & input,
                                    const Tensor & weight,
                                    const c10::optional<Tensor> & bias,
                                    IntArrayRef stride,
                                    IntArrayRef padding,
                                    IntArrayRef dilation,
                                    bool transposed,
                                    IntArrayRef /*output_padding*/,
                                    int64_t groups)
    {
        TORCH_CHECK(stride.size()==2 && padding.size() == 2 && dilation.size() == 2,"Expecting size of parameters=2");
        TORCH_CHECK(!transposed,"Transposed not implemeted yet");
        dlprim::Tensor X = todp(input);
        dlprim::Tensor W = todp(weight);
        dlprim::Tensor B;
        TORCH_CHECK(X.shape().size() == 4,"Invalid input shape");
        TORCH_CHECK(W.shape().size() == 4,"Invalid input shape");
        bool with_bias = bias && bias->numel() != 0;
        if(with_bias) {
            B=todp(*bias);
        }
        dlprim::Convolution2DConfigBase cfg_base;
        cfg_base.channels_in = X.shape()[1];
        cfg_base.channels_out = W.shape()[0];
        for(int i=0;i<2;i++) {
            cfg_base.kernel[i] = W.shape()[i+2];
            cfg_base.pad[i] = padding[i];
            cfg_base.stride[i] = stride[i];
            cfg_base.dilate[i] = dilation[i];
            cfg_base.groups = groups;
        }
        dlprim::core::Conv2DSettings cfg(cfg_base,X.shape(),X.dtype());
        dlprim::ExecutionContext q = getExecutionContext(input);
        dlprim::Context ctx(q);
        auto conv = dlprim::core::Conv2DForward::create(ctx,cfg,with_bias);
        size_t ws_size = conv->workspace();
        dlprim::Tensor ws;
        at::DataPtr ws_ptr;
        if(ws_size) {
            ws_ptr = std::move(CLContextManager::allocate(input.device(),ws_size));
            ws=dlprim::Tensor(cl::Buffer((cl_mem)ws_ptr.get(),true),0,dlprim::Shape(ws_size),dlprim::uint8_data);
        }
        
        dlprim::Shape rs = dlprim::core::Conv2DForward::get_output_shape(cfg,X.shape());

        int64_t int_shape[4]={int64_t(rs[0]),int64_t(rs[1]),int64_t(rs[2]),int64_t(rs[3])};

        torch::Tensor result = new_ocl_tensor(c10::IntArrayRef(int_shape,4),
                                              input.device(),
                                              input.dtype().toScalarType());
        dlprim::Tensor Y = todp(result);
        conv->enqueue(X,W,(with_bias ? &B : nullptr),Y,ws,0,q);
        sync_if_needed(input.device());

        return result;
    }

    Tensor & relu_(Tensor & self)
    {
        dlprim::Tensor X = todp(self);
        dlprim::ExecutionContext q = getExecutionContext(self);
        dlprim::core::activation_forward(X,X,dlprim::StandardActivations::relu,q);
        sync_if_needed(self.device());
        return self;
    }
    ::std::tuple<Tensor &,Tensor &> max_pool2d_with_indices_out(const Tensor & self, IntArrayRef kernel_size, IntArrayRef stride, IntArrayRef padding, IntArrayRef dilation, bool ceil_mode, Tensor & out, Tensor & indices)
    {
#warning "FIXME add indices support"
        dlprim::Tensor X = todp(self);
        dlprim::Tensor Y = todp(out);
        dlprim::ExecutionContext q = getExecutionContext(self);
        dlprim::Context ctx(q);
        TORCH_CHECK(kernel_size.size()==2 && stride.size()==2 && padding.size()==2 && dilation.size()==2,"Invalid sizes");
        TORCH_CHECK(dilation[0]==1 && dilation[1]==1,"Dilation != 1 is not implemented yet");
        TORCH_CHECK(ceil_mode==false,"ceil mode=true not implemeted yet");
        int kernel[2]={int(kernel_size[0]),int(kernel_size[1])};
        int pad[2]={int(padding[0]),int(padding[1])};
        int strd[2] = {int(stride[0]),int(stride[1])};
        auto pool = dlprim::core::Pooling2DForward::create_max_pooling(ctx,kernel,pad,strd,todp(self.dtype()));
        pool->enqueue(X,Y,q);
        sync_if_needed(self.device());

        return std::tuple<Tensor &,Tensor &>(out,indices);
    }
    Tensor _adaptive_avg_pool2d(const Tensor & self, IntArrayRef output_size) // {"schema": "aten::_adaptive_avg_pool2d
    {
        dlprim::Tensor X = todp(self);
        int h=X.shape()[2];
        int w=X.shape()[3];
        TORCH_CHECK((output_size[0]==1 && output_size[1]==1) || (output_size[0]==h && output_size[1]==w),"Only global pooling or no-pooling supported");
        if(output_size[0]==1 && output_size[1] == 1) {
            int64_t int_shape[4]={int64_t(X.shape()[0]),int64_t(X.shape()[1]),1,1};
            torch::Tensor result = new_ocl_tensor(c10::IntArrayRef(int_shape,4),
                    self.device(),
                    self.dtype().toScalarType());
            dlprim::Tensor Y = todp(result);

            dlprim::ExecutionContext q = getExecutionContext(self);
            dlprim::Context ctx(q);
            auto pool = dlprim::core::Pooling2DForward::create_global_avg_pooling(ctx,X.shape(),todp(self.dtype()));
            pool->enqueue(X,Y,q);
            sync_if_needed(self.device());
            return result;
        }
        else {
            return self;
        }

    }
   
    Tensor linear(const Tensor & input, const Tensor & weight, const c10::optional<Tensor> & bias)
    {
        dlprim::Tensor X = todp(input);
        dlprim::Tensor W = todp(weight);
        int64_t os[2]={int64_t(X.shape()[0]),int64_t(W.shape()[0])};
        Tensor result = new_ocl_tensor(c10::IntArrayRef(os,2),input.device(),input.dtype().toScalarType());
        dlprim::Tensor Y = todp(result);
        dlprim::ExecutionContext q = getExecutionContext(input);
        dlprim::Context ctx(q);
        dlprim::core::IPSettings cfg;
        cfg.inputs = X.shape().size_no_batch();
        cfg.outputs = W.shape()[0];
        cfg.optimal_batch_size = X.shape()[0];
        cfg.dtype = todp(input.dtype());
        bool has_bias = bias && bias->numel() > 0;
        auto ip = dlprim::core::IPForward::create(ctx,cfg,has_bias);
        dlprim::Tensor B;
        if(has_bias)
            B=todp(*bias);
        ip->enqueue(X,W,(has_bias ? &B : nullptr),Y,q);
        sync_if_needed(input.device());
        return result;
    }
    Tensor as_strided(const Tensor & self, IntArrayRef size, IntArrayRef /*stride*/, c10::optional<int64_t> /*storage_offset*/)
    {
        Tensor result = new_ocl_tensor(size,self.device(),self.dtype().toScalarType());
        dlprim::Tensor X = todp(self);
        dlprim::Tensor Y = todp(result);
        dlprim::ExecutionContext q = getExecutionContext(self);
        q.queue().enqueueCopyBuffer(X.device_buffer(),Y.device_buffer(),X.device_offset(),Y.device_offset(),X.memory_size(),q.events(),q.event("copy"));
        sync_if_needed(self.device());
        return result;

    }
} // namespace

TORCH_LIBRARY_IMPL(aten, PrivateUse1, m) {
      m.impl("aten::empty.memory_format", &ptdlprim::allocate_empty);
      m.impl("aten::_reshape_alias",&ptdlprim::_reshape_alias);
      m.impl("aten::fill_.Scalar",&ptdlprim::fill_);
      m.impl("aten::zero_",&ptdlprim::zero_);
      m.impl("aten::_copy_from",&ptdlprim::_copy_from);
      m.impl("aten::empty_strided",&ptdlprim::empty_strided);
      m.impl("aten::convolution_overrideable",&ptdlprim::convolution_overrideable);
      m.impl("aten::relu_",&ptdlprim::relu_);
      m.impl("aten::max_pool2d_with_indices.out",&ptdlprim::max_pool2d_with_indices_out);
      m.impl("aten::_adaptive_avg_pool2d",&ptdlprim::_adaptive_avg_pool2d);
      //m.impl("aten::as_strided",&ptdlprim::as_strided);
      //m.impl("aten::linear",&ptdlprim::linear);
}

TORCH_LIBRARY_IMPL(aten, AutogradPrivateUse1, m) {
      m.impl("aten::linear",&ptdlprim::linear);
}
