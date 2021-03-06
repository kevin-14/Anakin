#include "core/context.h"
#include "funcs/deconv.h"
#include "test_saber_func.h"
#include "test_saber_base.h"
#include "tensor_op.h"
#include "funcs/funcs_utils.h"
#include "saber_types.h"
#include <vector>
#include "debug.h"
#include "test/saber/conv_func_helper.h"
#ifdef USE_X86_PLACE
#include "saber/funcs/impl/x86/x86_utils.h"
#include "omp.h"
#endif
using namespace anakin::saber;

void fill_bias_relu(float* tensor, const float* bias, int channel, int channel_size,
                    bool flag_relu) {
    float* data = tensor;

    for (int j = 0; j < channel; ++j) {
        for (int i = 0; i < channel_size; i++) {
            data[i] += bias[j];

            if (flag_relu) {
                data[i] = data[i] > 0 ? data[i] : 0.f;
            }
        }

        data += channel_size;
    }
}

void fill_relu(float* tensor, int channel, int channel_size,
               bool flag_relu) {
    float* data = tensor;

    for (int j = 0; j < channel; ++j) {
        for (int i = 0; i < channel_size; i++) {
            if (flag_relu) {
                data[i] = data[i] > 0 ? data[i] : 0.f;
            }
        }

        data += channel_size;
    }
}

template<typename Dtype>
static void gemm_naive(int m, int n, int k, const float alpha, const Dtype* a, const Dtype* b,
                       const float beta, Dtype* c, bool trans_a = false, bool trans_b = false) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            Dtype acc = 0;

            for (int inner = 0; inner < k; inner++) {
                if (trans_a == false && trans_b == false) {
                    acc += alpha * a[i * k + inner] * b[inner * n + j];
                } else if (trans_a == true && trans_b == false) {
                    acc += alpha * a[inner * m + i] * b[inner * n + j];
                } else if (trans_a == false && trans_b == true) {
                    acc += alpha * a[i * k + inner] * b[j * k + inner];
                } else if (trans_a == true && trans_b == true) {
                    acc += alpha * a[i * k + inner] * b[j * k + inner];
                }
            }

            c[i * n + j] = acc + beta * c[i * n + j];
        }
    }
}

inline bool is_a_ge_zero_and_a_lt_b(int a, int b) {
    return static_cast<unsigned>(a) < static_cast<unsigned>(b);
}

template <typename Dtype>
void col2im(const Dtype* data_col, const int channels,
            const int height, const int width, const int kernel_h, const int kernel_w,
            const int pad_h, const int pad_w,
            const int stride_h, const int stride_w,
            const int dilation_h, const int dilation_w,
            Dtype* data_im) {

    memset(data_im, 0, height * width * channels * sizeof(Dtype));
    const int output_h = (height + 2 * pad_h - (dilation_h * (kernel_h - 1) + 1)) / stride_h + 1;
    const int output_w = (width + 2 * pad_w - (dilation_w * (kernel_w - 1) + 1)) / stride_w + 1;
    const int channel_size = height * width;

    for (int channel = channels; channel--; data_im += channel_size) {
        for (int kernel_row = 0; kernel_row < kernel_h; kernel_row++) {
            for (int kernel_col = 0; kernel_col < kernel_w; kernel_col++) {
                int input_row = -pad_h + kernel_row * dilation_h;

                for (int output_rows = output_h; output_rows; output_rows--) {
                    if (!is_a_ge_zero_and_a_lt_b(input_row, height)) {
                        data_col += output_w;
                    } else {
                        int input_col = -pad_w + kernel_col * dilation_w;

                        for (int output_col = output_w; output_col; output_col--) {
                            if (is_a_ge_zero_and_a_lt_b(input_col, width)) {
                                data_im[input_row * width + input_col] += *data_col;
                            }

                            data_col++;
                            input_col += stride_w;
                        }
                    }

                    input_row += stride_h;
                }
            }
        }
    }
}

template<typename dtype, typename TargetType_D, typename TargetType_H>
void gemm_transpose_conv(const std::vector<Tensor<TargetType_H>*>& inputs,
                         std::vector<Tensor<TargetType_H>*>& outputs,
                         ConvParam<TargetType_D>& param) {

    bool bias_term = param.bias() != nullptr && param.bias()->valid_size() > 0;
    Tensor<TargetType_H> weights_host;
    Tensor<TargetType_H> bias_host;
    weights_host.re_alloc(param.weight()->valid_shape(), AK_FLOAT);
    weights_host.copy_from(*(param.weight()));

    if (bias_term) {
        bias_host.re_alloc(param.bias()->valid_shape(), AK_FLOAT);
        bias_host.copy_from(*(param.bias()));
    }

    int win = inputs[0]->width();
    int hin = inputs[0]->height();
    int chin = inputs[0]->channel();
    int num = inputs[0]->num();
    int wout = outputs[0]->width();
    int hout = outputs[0]->height();
    int chout = outputs[0]->channel();

    int _kw = weights_host.width();
    int _kh = weights_host.height();

    int _m = chout * _kw * _kh / param.group;
    int _n = hin * win;
    int _k = chin / param.group;

    if (chin != chout || param.group != chin) {
        CHECK_EQ(chin % param.group, 0) << "input channel or group size error";
        CHECK_EQ(chout % param.group, 0) << "output channel or group size error";
    }

    Tensor<TargetType_H> workspace_tensor;
    Shape workspace_shape({1, 1, 1, param.group* _m * _n});
    workspace_tensor.re_alloc(workspace_shape, AK_FLOAT);

    int group = param.group;
    int group_size_in = win * hin * chin / group;
    int group_size_out = wout * hout * chout / group;
    int group_size_coldata = _m * _n;
    int group_size_weights = chin * chout * _kw * _kh / (group * group);
    bool flag_1x1s1p1 = (_kw == 1) && (_kh == 1) && (param.stride_h == 1) && \
                        (param.stride_w == 1) && (param.pad_w == 1) && (param.pad_h == 1) && \
                        (param.dilation_w == 1) && (param.dilation_h == 1);

    bool with_relu = param.activation_param.active == Active_relu;
    const float* din = static_cast<const float*>(inputs[0]->data());
    float* dout = static_cast<float*>(outputs[0]->mutable_data());
    const float* weights = static_cast<const float*>(weights_host.data());
    float* workspace_ptr = static_cast<float*>(workspace_tensor.mutable_data());

    for (int i = 0; i < num; ++i) {
        const float* din_batch = din + i * chin * hin * win;
        float* dout_batch = dout + i * chout * hout * wout;

        float* col_data = workspace_ptr;

        for (int g = 0; g < param.group; ++g) {
            const float* din_group = din_batch + g * group_size_in;
            const float* weights_group = weights + g * group_size_weights;
            float* coldata_group = col_data + g * group_size_coldata;
            gemm_naive(_m, _n, _k, 1.f, weights_group, din_group, 0.f, coldata_group, true, false);
        }

        col2im(col_data, chout, hout, wout, _kh, _kw, param.pad_h, param.pad_w, \
               param.stride_h, param.stride_w, param.dilation_h, param.dilation_w, \
               dout_batch);

        //! add bias
        if (bias_term) {
            fill_bias_relu(dout_batch, static_cast<const float*>(bias_host.data()), chout, wout * hout,
                           with_relu);
        } else {
            fill_relu(dout_batch, chout, wout * hout,
                      with_relu);
        }
    }
}

template <typename HOST, typename DEVICE>
void deconv_test(int img_n = 1,
                 int kernel_size = 3,
                 int pad = 0,
                 int stride = 1,
                 int img_h = 2,
                 int img_w = 2,
                 int in_channel = 1,
                 int out_channel = 1, // this actully the channel dim
                 bool bias_term = true,
                 ImplEnum impl = SABER_IMPL) {

    typedef Tensor<HOST> TensorHf4;
    typedef Tensor<DEVICE> TensorDf4;
    Context<DEVICE> ctx_dev(0, 1, 1);
    TensorHf4 img_host;
    TensorDf4 img_dev;
    std::vector<TensorDf4*> inputs;
    std::vector<TensorHf4*> inputs_host;

    TensorHf4 weights_origin_host;
    TensorDf4 weights_target;

    Shape img_shape({img_n, in_channel, img_h, img_w});
    Shape weights_shape({in_channel, out_channel, kernel_size, kernel_size});

    img_host.re_alloc(img_shape, AK_FLOAT);
    img_dev.re_alloc(img_shape, AK_FLOAT);

    fill_tensor_rand(img_host, -2.f, 2.f);
    //    fill_tensor_const(img_host, 1);
    img_dev.copy_from(img_host);
    inputs.push_back(&img_dev);
    inputs_host.push_back(&img_host);

    weights_origin_host.re_alloc(weights_shape, AK_FLOAT);
    weights_target.re_alloc(weights_shape, AK_FLOAT);

    fill_tensor_rand(weights_origin_host, -1.f, 1.f);
    weights_target.copy_from(weights_origin_host);

    TensorDf4 bias_target;
    TensorHf4 bias_host;
    Shape bias_shape({1, out_channel, 1, 1});

    if (bias_term) {
        bias_host.re_alloc(bias_shape, AK_FLOAT);
        bias_target.re_alloc(bias_shape, AK_FLOAT);
        fill_tensor_const(bias_host, 1);
        bias_target.copy_from(bias_host);
    }

    TensorDf4 target_out_tensor;
    TensorHf4 host_out_tensor;
    std::vector<TensorDf4*> target_outputs;
    std::vector<TensorHf4*> host_outputs;
    target_outputs.push_back(&target_out_tensor);
    host_outputs.push_back(&host_out_tensor);

    ConvParam<HOST> conv_param(1, pad, pad,
                               stride, stride,
                               1, 1,
                               &weights_origin_host, &bias_host);

    ConvParam<DEVICE> conv_param_target(1, pad, pad,
                                        stride, stride,
                                        1, 1,
                                        &weights_target, &bias_target);

    Deconv<DEVICE, AK_FLOAT> deconv;
    deconv.compute_output_shape(inputs, target_outputs, conv_param);
    target_outputs[0]->re_alloc(target_outputs[0]->shape(), AK_FLOAT);

    SABER_CHECK(deconv.init(inputs, target_outputs, conv_param, SPECIFY, impl, ctx_dev));

    deconv(inputs, target_outputs, conv_param, ctx_dev);
    target_outputs[0]->record_event(ctx_dev.get_compute_stream());
    target_outputs[0]->sync();

};

template<typename TargetType, typename TargetType_H>
int test_deconv_results_x86_C8R(int group,
                                int input_num, int in_channels, int height, int width,
                                int out_channels, int kernel_h, int kernel_w,
                                int stride_h, int stride_w, int dilation_h, int dilation_w,
                                int pad_h, int pad_w, bool bias_term, bool with_relu,
                                SaberImplStrategy strategy, ImplEnum imp) {

    LOG(INFO) << " conv param: "
              << " input_num = " << input_num
              << " in_channels = " << in_channels
              << " height = " << height
              << " width = " << width
              << " group = " << group
              << " pad_h = " << pad_h
              << " pad_w = " << pad_w
              << " stride_h = " << stride_h
              << " stride_w = " << stride_w
              << " dilation_h = " << dilation_h
              << " dilation_w = " << dilation_w
              << " kernel_h = " << kernel_h
              << " kernel_w = " << kernel_w
              << " out_channels = " << out_channels
              << " bias_term = " << (bias_term ? "true" : "false")
              << " with_relu = " << (with_relu ? "true" : "false");

    Shape input_s({input_num, in_channels, height, width}, Layout_NCHW_C8R);
    Shape weights_s({out_channels, in_channels / group, kernel_h, kernel_w}, Layout_NCHW);
    Shape bias_s({1, out_channels, 1, 1}, Layout_NCHW);
    int kernel_extent_h = dilation_h *
                          (kernel_h - 1) + 1;
    int output_dim_h = (height - 1) *
                       stride_h + kernel_extent_h - 2 * pad_h;
    int kernel_extent_w = dilation_w *
                          (kernel_w - 1) + 1;
    int output_dim_w = (width - 1) *
                       stride_w + kernel_extent_w - 2 * pad_w;
    int out_height = output_dim_h;
    int out_width = output_dim_w;
    Shape output_dev_s({input_num, out_channels, out_height, out_width}, Layout_NCHW_C8R);
    // init input Tensor
    Tensor<TargetType> input_dev;
    Tensor<TargetType_H> input_host;
    input_dev.re_alloc(input_s, AK_FLOAT);
    input_host.re_alloc(input_s, AK_FLOAT);
    //    {
    //        float *tmp= static_cast<float*>(input_dev.mutable_data());
    //        for(int i=0;i<height;i++){
    //            for(int j=0;j<width;j++){
    //                for(int c=0;c<8;c++){
    //                    int index=i*width*8+j*8+c;
    //                    tmp[index]=i*width+j;
    //                }
    //            }
    //
    //        }
    //    }

    //        fill_tensor_const(input_dev, 1.f);
    //    fill_tensor_seq(input_dev);
    fill_tensor_rand(input_dev, -2.0f, 2.0f);
    input_host.copy_from(input_dev);


    // init weights Tensor
    Tensor<TargetType> weights_dev;
    Tensor<TargetType_H> weights_host;
    weights_dev.re_alloc(weights_s, AK_FLOAT);
    weights_host.re_alloc(weights_s, AK_FLOAT);
    fill_tensor_const(weights_dev, 1.f);
    //    fill_tensor_seq(weights_dev);
    //    fill_tensor_rand(weights_dev, -2.0f, 2.0f);
    weights_host.copy_from(weights_dev);

    Tensor<TargetType> bias_dev;
    Tensor<TargetType_H> bias_host;

    if (bias_term) {
        bias_dev.re_alloc(bias_s, AK_FLOAT);
        bias_host.re_alloc(bias_s, AK_FLOAT);
        fill_tensor_const(bias_dev, -1.f);
        //        fill_tensor_rand(bias_dev, -2.0f, 2.0f);
        bias_host.copy_from(bias_dev);
    }

    Tensor<TargetType> output_dev(output_dev_s);
    Tensor<TargetType_H> output_host(output_dev_s);
    Tensor<TargetType_H> check_host;
    fill_tensor_const(output_dev, -10.f);
    Context<TargetType> ctx1(0, 1, 1);
    //    ActivationParam<TargetType> act_param(Active_relu);
    ConvParam<TargetType> param(group, pad_h, pad_w,
                                stride_h, stride_w,
                                dilation_h, dilation_w,
                                &weights_dev, &bias_dev);

    if (with_relu) {
        ActivationParam<TargetType> act_param(Active_relu);
        param.activation_param = act_param;
    }

    Deconv<TargetType, AK_FLOAT> conv;
    std::vector<Tensor<TargetType>* > input_v;
    std::vector<Tensor<TargetType>* > output_v;
    input_v.push_back(&input_dev);
    output_v.push_back(&output_dev);
    //    output_dev.set_layout_without_shape(Layout_NCHW_C8);
    conv.compute_output_shape(input_v, output_v, param);
    //            LOG(INFO)<<"layout "<<output_dev.get_layout();
    //    output_dev.re_alloc(output_dev.valid_shape(), AK_FLOAT);
    //    output_host.re_alloc()
    //    output_dev.re_alloc(output_dev_s, AK_FLOAT);

    //            LOG(INFO)<<"layout "<<output_dev.get_layout();

    SABER_CHECK(conv.init(input_v, output_v, param, strategy, imp, ctx1));

    //            LOG(INFO)<<"layout "<<output_dev.get_layout()<<","<<output_dev.size()<<","<<output_dev.valid_size();
    SABER_CHECK(conv(input_v, output_v, param, ctx1));

    //    LOG(INFO)<<"conv finish";
    typename Tensor<TargetType>::API::stream_t stream = ctx1.get_compute_stream();
    //    output_v[0]->record_event(stream);
    //    output_v[0]->sync();
    //    output_host.re_alloc(output_dev.valid_shape(), AK_FLOAT);
    //    output_host.copy_from(output_dev);

    //    print_tensor(input_dev);
    //    print_tensor(output_dev);
    //    print_tensor(output_host);
    Tensor<TargetType_H> nchwc8_input_check(Shape({input_num, in_channels, height, width}));
    anakin::saber::reorder_nchwc_nchw(input_host, nchwc8_input_check);
    check_host.re_alloc(Shape({input_num, out_channels, out_height, out_width}), AK_FLOAT);
    Tensor<TargetType_H> nchw_output_check(check_host.valid_shape());
    std::vector<Tensor<TargetType_H>*> check_in_vec {&nchwc8_input_check};
    std::vector<Tensor<TargetType_H>*> check_out_vec {&check_host};
    gemm_transpose_conv<TargetType_H>(check_in_vec, check_out_vec, param);
    LOG(INFO) << "cal check finish";
    //    print_tensor_valid(check_host);

    //    anakin::saber::input_reorder_nChwc8(check_host,nchw_output_check);
    Tensor<TargetType_H> nchwc8_output_check(check_host.valid_shape());
    anakin::saber::reorder_nchwc_nchw(output_dev, nchwc8_output_check);
    double max_ratio = 0.0;
    double max_diff = 0.0;
    tensor_cmp_host((const float*)nchwc8_output_check.data(), (const float*)check_host.data(),
                    check_host.valid_size(), max_ratio, max_diff);

    if (max_ratio > 1e-3 && max_diff > 1e-3) {
        //        print_tensor(nchwc8_output_check);
        //        print_tensor(check_host);

        //        print_tensor(input_host);
        //        print_tensor(weights_dev);
        LOG(FATAL) << " max_ratio = " << max_ratio << " max_diff = " << max_diff;
    } else {
        LOG(INFO) << "passed";
    }

    return 0;
}

template <typename HOST, typename DEVICE>
void deconv_testbase() {
    Env<DEVICE>::env_init();
    Env<HOST>::env_init();
    TestSaberBase<DEVICE, HOST, AK_FLOAT, Deconv, ConvParam> testbase;
    //    std::vector<int> kernel{3,4,5,6,7};
    //    std::vector<int> pad{0,1,2};
    //    std::vector<int> stride{1,2,3};
    std::vector<int> kernel {3, 4, 5, 6, 7};
    std::vector<int> pad {0, 1};
    std::vector<int> stride {2};
    std::vector<int> dilation_v {1};
    std::vector<int> group_v {1};
    std::vector<int> in_h_v {22};
    std::vector<int> in_w_v {23};
    std::vector<int> input_num_v {1};
    std::vector<int> input_channels_v {12};
    std::vector<int> output_channels_v {21};
    std::vector<bool> bias_term_v {true, false};
    std::vector<bool> with_relu_v {true, false};

    for (auto relu_flag : with_relu_v)
        for (auto kernel_h : kernel)
            for (auto pad_h : pad)
                for (auto stride_h : stride)
                    for (auto dilation_h : dilation_v)
                        for (auto bias_term : bias_term_v)
                            for (auto in_channels : input_channels_v)
                                for (auto out_channels : output_channels_v)
                                    for (auto group : group_v) {
                                        auto kernel_w = kernel_h;
                                        auto pad_w = pad_h;
                                        auto stride_w = stride_h;
                                        int dilation_w = dilation_h;

                                        Shape weights_s({out_channels, in_channels, kernel_h, kernel_w}, Layout_NCHW);
                                        Shape bias_s({1, out_channels, 1, 1}, Layout_NCHW);

                                        Tensor<DEVICE> weights_dev;
                                        Tensor<DEVICE> bias_dev;

                                        weights_dev.re_alloc(weights_s, AK_FLOAT);
                                        fill_tensor_rand(weights_dev, -2.f, 2.0f);
                                        //        fill_tensor_const(weights_dev,1.f);

                                        if (bias_term) {
                                            bias_dev.re_alloc(bias_s, AK_FLOAT);
                                            fill_tensor_rand(bias_dev, -1.f, 1.f);
                                        }

                                        ConvParam<DEVICE> param_nv(group, pad_h, pad_w,
                                                                   stride_h, stride_w,
                                                                   dilation_h, dilation_w,
                                                                   &weights_dev, &bias_dev);

                                        if (relu_flag) {
                                            param_nv.activation_param = ActivationParam<DEVICE>(Active_relu);
                                        }

                                        for (auto input_num : input_num_v)
                                            for (auto height : in_h_v)
                                                for (auto width : in_w_v) {
                                                    testbase.set_param(param_nv);//set param
                                                    testbase.set_rand_limit(-1.f, 1.f);
                                                    //            testbase.set_rand_limit(1.f,1.f);
                                                    testbase.set_input_shape(Shape({input_num, in_channels, height, width},
                                                                                   Layout_NCHW));//add some input shape
                                                    LOG(INFO) << kernel_h << "," << kernel_w << "," << pad_h << "," << pad_w << "," << stride_h << ","
                                                              << stride_w << ", [" << input_num << "," << in_channels << "," << height << "," << width << "] ," <<
                                                              out_channels << ",bias = " << bias_term;

                                                    if (std::is_same<DEVICE, MLU>::value) {
                                                        testbase.run_test(gemm_transpose_conv<float, DEVICE, HOST>, 0.02, true);//run test
                                                    } else {
                                                        testbase.run_test(gemm_transpose_conv<float, DEVICE, HOST>, 1e-3, true);//run test}
                                                    }
                                                }
                                    }
}

TEST(TestSaberFunc, test_func_self_deconv_nv) {
#ifdef NVIDIA_GPU
    deconv_testbase<NVHX86, NV>();
#endif
}

TEST(TestSaberFunc, test_func_self_deconv_x86) {
#ifdef USE_X86_PLACE
    Env<X86>::env_init();
    int group = 1;
    int input_num = 1;
    int in_channels = 8;
    int height = 3;
    int width = 3;
    int out_channels = 16;
    int kernel_h = 3;
    int kernel_w = 3;
    int stride_h = 2;
    int stride_w = 2;
    int dilation_h = 1;
    int dilation_w = 1;
    int pad_h = 0;
    int pad_w = 0;
    bool bias_term = false;
    bool with_relu = false;

    //    int group = 1;
    //    int input_num = 1;
    //    int in_channels = 16;
    //    int height = 15;
    //    int width = 28;
    //    int out_channels = 16;
    //    int kernel_h = 3;
    //    int kernel_w = 3;
    //    int stride_h = 2;
    //    int stride_w = 2;
    //    int dilation_h = 1;
    //    int dilation_w = 1;
    //    int pad_h = 0;
    //    int pad_w = 0;
    //    bool bias_term = true;
    //    bool with_relu = false;
    test_deconv_results_x86_C8R<X86, X86>(group,
                                          input_num, in_channels,
                                          height, width,
                                          out_channels, kernel_h,
                                          kernel_w,
                                          stride_h, stride_w,
                                          dilation_h, dilation_w,
                                          pad_h, pad_w, bias_term,
                                          with_relu,
                                          SPECIFY, SABER_IMPL);
#endif
}

TEST(TestSaberFunc, test_func_self_deconv_mlu) {
#if 0
#ifdef USE_MLU
    Env<MLUHX86>::env_init();
    Env<MLU>::env_init();
    deconv_testbase<MLUHX86, MLU>();
#endif
#endif
}


int main(int argc, const char** argv) {
    // initial logger
    //logger::init(argv[0]);

    InitTest();
    RUN_ALL_TESTS(argv[0]);
    return 0;
}

