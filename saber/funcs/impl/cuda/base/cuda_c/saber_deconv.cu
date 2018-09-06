#include "saber/funcs/impl/cuda/saber_deconv.h"
#include "saber/funcs/saber_util.h"


namespace anakin{

namespace saber{

template <typename Dtype,bool with_relu>
static __global__ void ker_bias_relu(Dtype* tensor,const Dtype* bias,int channel_num,int channel_size){
    const int thread_id = blockIdx.x*blockDim.x+threadIdx.x;
    const int channel_id=thread_id/channel_size;
    const int channel_inner_index=thread_id%channel_size;
    if(channel_id<channel_num){
        Dtype tmp=tensor[thread_id]+bias[channel_id];
        if(with_relu){
            tensor[thread_id]=tmp>0?tmp:0;
        }else{
            tensor[thread_id]=tmp;
        }
    }
};

template <typename Dtype>
static inline void bias_relu(Dtype* tensor,const Dtype* bias,int channel_num,int channel_size,int with_relu,cudaStream_t stream){
    if(with_relu){
        ker_bias_relu<Dtype,true><<<CUDA_GET_BLOCKS(channel_num*channel_size),
                CUDA_NUM_THREADS,0,stream>>>(tensor,bias,channel_num,channel_size);
    }else{
        ker_bias_relu<Dtype,true><<<CUDA_GET_BLOCKS(channel_num*channel_size),
                CUDA_NUM_THREADS,0,stream>>>(tensor,bias,channel_num,channel_size);
    }
}

template <typename Dtype>
static __global__ void col2im_gpu_kernel(const int n, const Dtype* data_col,
                                  const int height, const int width, const int channels,
                                  const int kernel_h, const int kernel_w,
                                  const int pad_h, const int pad_w,
                                  const int stride_h, const int stride_w,
                                  const int dilation_h, const int dilation_w,
                                  const int height_col, const int width_col,
                                  Dtype* data_im) {
    CUDA_KERNEL_LOOP(index, n) {
        Dtype val = 0;
        const int w_im = index % width + pad_w;
        const int h_im = (index / width) % height + pad_h;
        const int c_im = index / (width * height);
        int kernel_extent_w = (kernel_w - 1) * dilation_w + 1;
        int kernel_extent_h = (kernel_h - 1) * dilation_h + 1;
        // compute the start and end of the output
        const int w_col_start =
                (w_im < kernel_extent_w) ? 0 : (w_im - kernel_extent_w) / stride_w + 1;
        const int w_col_end = min(w_im / stride_w + 1, width_col);
        const int h_col_start =
                (h_im < kernel_extent_h) ? 0 : (h_im - kernel_extent_h) / stride_h + 1;
        const int h_col_end = min(h_im / stride_h + 1, height_col);
        // TODO: use LCM of stride and dilation to avoid unnecessary loops
        for (int h_col = h_col_start; h_col < h_col_end; h_col += 1) {
            for (int w_col = w_col_start; w_col < w_col_end; w_col += 1) {
                int h_k = (h_im - h_col * stride_h);
                int w_k = (w_im - w_col * stride_w);
                if (h_k % dilation_h == 0 && w_k % dilation_w == 0) {
                    h_k /= dilation_h;
                    w_k /= dilation_w;
                    int data_col_index = (((c_im * kernel_h + h_k) * kernel_w + w_k) *
                                          height_col + h_col) * width_col + w_col;
                    val += data_col[data_col_index];
                }
            }
        }
        data_im[index] = val;
    }
}

template <typename Dtype>
static void col2im_gpu(const Dtype* data_col, const int channels,
                const int height, const int width, const int kernel_h, const int kernel_w,
                const int pad_h, const int pad_w, const int stride_h,
                const int stride_w, const int dilation_h, const int dilation_w,
                Dtype* data_im,cudaStream_t stream) {
    int height_col = (height + 2 * pad_h - (dilation_h * (kernel_h - 1) + 1)) /
                     stride_h + 1;
    int width_col = (width + 2 * pad_w - (dilation_w * (kernel_w - 1) + 1)) /
                    stride_w + 1;
    int num_kernels = channels * height * width;
    // To avoid involving atomic operations, we will launch one kernel per
    // bottom dimension, and then in the kernel add up the top dimensions.
    // NOLINT_NEXT_LINE(whitespace/operators)
    col2im_gpu_kernel<Dtype><<<CUDA_GET_BLOCKS(num_kernels),
            CUDA_NUM_THREADS,0,stream>>>(
                    num_kernels, data_col, height, width, channels, kernel_h, kernel_w,
                            pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w,
                            height_col, width_col, data_im);
}

template <typename dtype, bool flag_bias, bool flag_act>
__global__ void direct_deconv(const dtype* const din,
                              const dtype* bias_data, const dtype* const weight_data,
                              const int num, const int in_channels, const int out_channels,
                              const int hout,const int wout, const int channel_out_stride,
                              const int hin, const int win, const int channel_in_stride,
                              const int kernel_h, const int kernel_w, const int kernel_size,
                              const int stride_h, const int stride_w,
                              const int pad_h, const int pad_w,
                              const int dilation_h, const int dilation_w,
                              dtype* dout) {

    int wo = blockIdx.x * blockDim.x + threadIdx.x;
    int w =  wo + pad_w;
    int ho = blockIdx.y * blockDim.y + threadIdx.y;
    int h =  ho + pad_h;
    int iout = blockIdx.z;
    int cout = iout % out_channels;
    int n = iout / out_channels;
    int iin = n * in_channels;
    int idx_out = iout * channel_out_stride + ho * wout + wo;

    extern __shared__ dtype sharedw[];

    dtype val = 0;

    if (wo < wout && ho < hout) {
        for(int ic = 0; ic < in_channels; ic++) {
            //! read weights
            int idx_weight = threadIdx.y * blockDim.x + threadIdx.x;
            if (idx_weight < kernel_size) {
                sharedw[idx_weight] = weight_data[(ic * out_channels + cout) * kernel_size + idx_weight];
            }
            __syncthreads();
            //! get start and end index
            const int phstart = (h < kernel_h) ? 0 : (h - kernel_h) / stride_h + 1;
            const int phend = min(h / stride_h + 1, hin);
            const int pwstart = (w < kernel_w) ? 0 : (w - kernel_w) / stride_w + 1;
            const int pwend = min(w / stride_w + 1, win);

            const int khstart = (h >= kernel_h) ? ((h - kernel_h) % stride_h) + (kernel_h - stride_h) : h;
            const int kwstart = (w >= kernel_w) ? ((w - kernel_w) % stride_w) + (kernel_w - stride_w) : w;

            const dtype* const din_c = din + (iin + ic) * channel_in_stride;

            //! start computation
            for (int ph = phstart; ph < phend; ++ph) {
                for (int pw = pwstart; pw < pwend; ++pw) {
                    int kh = khstart - (ph - phstart) * stride_h;
                    int kw = kwstart - (pw - pwstart) * stride_w;
                    val += din_c[ph * win + pw] * sharedw[kh * kernel_w + kw];
                }
            }
        }
        //! finnal computation
        if (flag_bias) {
            val += bias_data[cout];
        }
        if (flag_act) {
            val = val > (dtype)0? val : (dtype)0;
        }
        dout[idx_out] = val;

    }
}

template <typename dtype, bool bias_flag, bool relu_flag>
__global__ void depthwise_deconv_2d(const int channel_in_stride, const int channel_out_stride,
                                    const int kernel_size,
                                    const dtype* const din, const int num, const int channels,
                                    const int hin, const int win, const int hout,
                                    const int wout, const int kernel_h, const int kernel_w,
                                    const int stride_h, const int stride_w, const int pad_h, const int pad_w,
                                    dtype* const dout, const dtype* const weight, const dtype* const bias) {

    int wo = blockIdx.x * blockDim.x + threadIdx.x;
    int w =  wo + pad_w;
    int ho = blockIdx.y * blockDim.y + threadIdx.y;
    int h =  ho + pad_h;
    int c = blockIdx.z % channels;
    //int n = blockIdx.z / channels;
    int i = blockIdx.z;
    int index = i * channel_out_stride + ho * wout + wo;

    extern __shared__ dtype sharedw[];
    int idx = threadIdx.y * blockDim.x + threadIdx.x;
    if (idx < kernel_size) {
        sharedw[idx] = weight[c * kernel_size + idx];
    }
    __syncthreads();

    if (wo < wout && ho < hout) {
        const int phstart = (h < kernel_h) ? 0 : (h - kernel_h) / stride_h + 1;
        const int phend = min(h / stride_h + 1, hin);
        const int pwstart = (w < kernel_w) ? 0 : (w - kernel_w) / stride_w + 1;
        const int pwend = min(w / stride_w + 1, win);

        const int khstart=(h >= kernel_h) ? ((h-kernel_h)%stride_h)+(kernel_h-stride_h): h;
        const int kwstart=(w >= kernel_w) ? ((w-kernel_w)%stride_w)+(kernel_w-stride_w) : w;

        dtype gradient = 0;
        const dtype* const top_diff_slice = din + i * channel_in_stride;

        const dtype* const weight_slice = weight + c * kernel_size;

        for (int ph = phstart; ph < phend; ++ph) {
            for (int pw = pwstart; pw < pwend; ++pw) {
                int kh = khstart - (ph - phstart) * stride_h;
                int kw = kwstart - (pw - pwstart) * stride_w;
                gradient += top_diff_slice[ph * win + pw] * sharedw[kh * kernel_w + kw];
                //gradient += top_diff_slice[ph * win + pw] * weight_slice[kh * kernel_w + kw];
            }
        }
        if (bias_flag) {
            gradient += bias[c];
        }
        if (relu_flag) {
            gradient = gradient > (dtype)0? gradient : (dtype)0;
        }
        dout[index] = gradient;
    }
}
template <>
SaberStatus SaberDeconv2D<NV, AK_FLOAT>::dispatch( \
        const std::vector<OpTensor*>& inputs,
        std::vector<OpTensor*>& outputs,
        ConvParam<NV>& param) {
    cudaStream_t stream = this->_ctx->get_compute_stream();
    //! inputs only has one tensor

    const float* din = static_cast<const float *>(inputs[0]->data());
    float* dout = static_cast<float *>(outputs[0]->mutable_data());
    const float* weight = static_cast<const float *>(param.weight()->data());

    int win = inputs[0]->width();
    int hin = inputs[0]->height();
    int num = inputs[0]->num();
    int ch_in = inputs[0]->channel();
    int wout = outputs[0]->width();
    int hout = outputs[0]->height();
    int ch_out = outputs[0]->channel();

    int kernel_w = param.weight()->width();
    int kernel_h = param.weight()->height();


    dim3 block(32, 32);
    int gx = (wout + block.x - 1) / block.x;
    int gy = (hout + block.y - 1) / block.y;
    dim3 grid(gx, gy, num * ch_out);
    int channel_in_stride = hin * win;
    int channel_out_stride = hout * wout;
    int kernel_size = kernel_h * kernel_w;
    int shared_mem_size = kernel_size * sizeof(float);

    if (_use_k4_s2_p1) {
        const float * bias_data = (param.bias()->valid_size() > 0) ?
                                  static_cast<const float *>(param.bias()->data()) : NULL;
        const float *weights_data = static_cast<const float *>(param.weight()->data());
        ker_deconv_implicit_gemm_k4_s2_p1_16x64(dout, din,
                                                weights_data, bias_data,
                                                num,
                                                hin, win, hout, wout,
                                                ch_in, ch_out, stream);
        return SaberSuccess;
    }


    int _m = ch_out * kernel_w * kernel_h / param.group;
    int _n = hin * win;
    int _k = ch_in / param.group;
    int group = param.group;
    int group_size_in = win * hin * ch_in / group;
    int group_size_out = wout * hout * ch_out / group;
    int group_size_coldata = _m * _n;
    int group_size_weights = ch_in * ch_out * kernel_w * kernel_h / (group * group);
    bool with_relu=param.activation_param.active==Active_relu;
    utils::try_expand_tensor(_workspace_tensor,param.group * _m * _n);
    float* workspace_ptr=static_cast<float*>(_workspace_tensor.mutable_data());
    const float * bias_data = (param.bias()!= nullptr&&param.bias()->valid_size() > 0) ?
                              static_cast<const float *>(param.bias()->data()) : nullptr;
    const float *weights_data = static_cast<const float *>(param.weight()->data());
    for (int i = 0; i < num; ++i) {
        const float* din_batch = din + i * ch_in * hin * win;
        float* dout_batch = dout + i * ch_out * hout * wout;

        float* col_data = workspace_ptr;

        for (int g = 0; g < param.group; ++g) {
            const float* din_group = din_batch + g * group_size_in;
            const float* weights_group = weights_data + g * group_size_weights;
            float* coldata_group = col_data + g * group_size_coldata;
            _gemm_wx(_m,_n,_k,1.f,weights_group, 0.f,din_group,coldata_group,stream);
        }


        col2im_gpu(col_data, ch_out, hout, wout, kernel_h, kernel_w, param.pad_h, param.pad_w, \
        param.stride_h, param.stride_w, param.dilation_h, param.dilation_w, \
        dout_batch,stream);

        //! add bias
        if (bias_data!= nullptr) {
            bias_relu(dout_batch,bias_data,ch_out,wout * hout,with_relu,stream);
        }
//        LOG(INFO)<<"CPU DECONV grout = "<<param.group;
//        print_tensor(weights_host);
//        print_tensor(bias_host);
//        print_tensor(*inputs[0]);
//        print_tensor(workspace_tensor);
//        print_tensor(*outputs[0]);
//        LOG(INFO)<<"CPU DECONV END";
    }
#if 0
    if (param.bias()!= nullptr&&param.bias()->valid_size() > 0) { // deconv with bias
        const float* bias = static_cast<const float*>(param.bias()->data());
        //! depthwise deconv
        if (param.group == ch_in && ch_in == ch_out) {
//            LOG(ERROR) << "In deconv cu";
            if(param.activation_param.has_active&&param.activation_param.active== Active_relu) {
                depthwise_deconv_2d<float, true, true> << < grid, block, shared_mem_size, stream >> > (
                        channel_in_stride, channel_out_stride, kernel_size, \
                din, num, ch_in, hin, win, hout, wout, kernel_h, \
                kernel_w, param.stride_h, param.stride_w, \
                param.pad_h, param.pad_w, \
                dout, weight, bias);
            }else{
                depthwise_deconv_2d<float, true, false> << < grid, block, shared_mem_size, stream >> > (
                        channel_in_stride, channel_out_stride, kernel_size, \
                din, num, ch_in, hin, win, hout, wout, kernel_h, \
                kernel_w, param.stride_h, param.stride_w, \
                param.pad_h, param.pad_w, \
                dout, weight, bias);
            }
        } else {
            if(param.activation_param.has_active&&param.activation_param.active== Active_relu) {
                direct_deconv<float, true, true> << < grid, block, shared_mem_size, stream >> > \
                (din, bias, weight, num, ch_in, ch_out, hout, wout, channel_out_stride, \
                hin, win, channel_in_stride, kernel_h, kernel_w, kernel_size, \
                param.stride_h, param.stride_w, param.pad_h, param.pad_w, \
                param.dilation_h, param.dilation_w, dout);
            }else{
                direct_deconv<float, true, false> << < grid, block, shared_mem_size, stream >> > \
                (din, bias, weight, num, ch_in, ch_out, hout, wout, channel_out_stride, \
                hin, win, channel_in_stride, kernel_h, kernel_w, kernel_size, \
                param.stride_h, param.stride_w, param.pad_h, param.pad_w, \
                param.dilation_h, param.dilation_w, dout);
            }
        }
    } else { //deconv without bias
        //! depthwise deconv
        if (param.group == ch_in && ch_in == ch_out) {
//            LOG(ERROR) << "In deconv cu";
            if(param.activation_param.has_active&&param.activation_param.active== Active_relu) {
                depthwise_deconv_2d<float, false, true> << < grid, block, shared_mem_size, stream >> > (
                        channel_in_stride, channel_out_stride, kernel_size, \
                din, num, ch_in, hin, win, hout, wout, kernel_h, \
                kernel_w, param.stride_h, param.stride_w, \
                param.pad_h, param.pad_w, \
                dout, weight, nullptr);
            }else{
                depthwise_deconv_2d<float, false, false> << < grid, block, shared_mem_size, stream >> > (
                        channel_in_stride, channel_out_stride, kernel_size, \
                din, num, ch_in, hin, win, hout, wout, kernel_h, \
                kernel_w, param.stride_h, param.stride_w, \
                param.pad_h, param.pad_w, \
                dout, weight, nullptr);
            }
        } else {
//            LOG(INFO)<<"Calling This ";
            if(param.activation_param.has_active&&param.activation_param.active== Active_relu) {
                direct_deconv<float, false, true> << < grid, block, shared_mem_size, stream >> > \
                (din, nullptr, weight, num, ch_in, ch_out, hout, wout, channel_out_stride, \
                hin, win, channel_in_stride, kernel_h, kernel_w, kernel_size, \
                param.stride_h, param.stride_w, param.pad_h, param.pad_w, \
                param.dilation_h, param.dilation_w, dout);
            }else{
                direct_deconv<float, false, false> << < grid, block, shared_mem_size, stream >> > \
                (din, nullptr, weight, num, ch_in, ch_out, hout, wout, channel_out_stride, \
                hin, win, channel_in_stride, kernel_h, kernel_w, kernel_size, \
                param.stride_h, param.stride_w, param.pad_h, param.pad_w, \
                param.dilation_h, param.dilation_w, dout);
            }
        }
    }
#endif

    return SaberSuccess;
}
template class SaberDeconv2D<NV, AK_FLOAT>;
DEFINE_OP_TEMPLATE(SaberDeconv2D, ConvParam, NV, AK_INT16);
DEFINE_OP_TEMPLATE(SaberDeconv2D, ConvParam, NV, AK_INT8);
} //namespace anakin

} //namespace anakin
