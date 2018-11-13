/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/image_ops.h"
#include "tensorflow/cc/ops/nn_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/common_runtime/kernel_benchmark_testlib.h"
#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/kernels/conv_ops_gpu.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/public/session.h"

namespace tensorflow {

#if GOOGLE_CUDA

struct ConvParametersPeer {
  template <typename T>
  bool ShouldIncludeWinogradNonfusedAlgoPreCudnn7() {
    return params.ShouldIncludeWinogradNonfusedAlgoPreCudnn7<T>();
  }

  ConvParameters params;
};

TEST(ConvParameters, WinogradNonfusedAlgoSize) {
  ConvParametersPeer conv_params_small = {{
      1,            // batch
      32,           // in_depths
      {{300,        // in_rows
        300}},      // in_cols
      FORMAT_NCHW,  // compute_data_format
      128,          // out_depths
      {{3,          // filter_rows
        3}},        // filter_cols
      {{1,          // dilation_rows
        1}},        // dilation_cols
      {{1,          // stride_rows
        1}},        // stride_cols
      {{0,          // padding_rows
        0}},        // padding_cols
      DT_FLOAT,     // tensor datatype
      0,            // device_id
  }};
  EXPECT_TRUE(
      conv_params_small.ShouldIncludeWinogradNonfusedAlgoPreCudnn7<float>());

  ConvParametersPeer conv_params_large = {{
      1,            // batch
      128,          // in_depths
      {{300,        // in_rows
        300}},      // in_cols
      FORMAT_NCHW,  // compute_data_format
      768,          // out_depths
      {{3,          // filter_rows
        3}},        // filter_cols
      {{1,          // dilation_rows
        1}},        // dilation_cols
      {{1,          // stride_rows
        1}},        // stride_cols
      {{0,          // padding_rows
        0}},        // padding_cols
      DT_FLOAT,     // tensor datatype
      0,            // device_id
  }};
  EXPECT_FALSE(
      conv_params_large.ShouldIncludeWinogradNonfusedAlgoPreCudnn7<float>());
}

#endif  // GOOGLE_CUDA

class FusedResizePadConvOpTest : public OpsTestBase {
 protected:
  template <typename T>
  void HandwrittenConv(DataType dtype) {
    const int stride = 1;
    TF_EXPECT_OK(NodeDefBuilder("fused_resize_op", "FusedResizeAndPadConv2D")
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(DT_INT32))
                     .Input(FakeInput(DT_INT32))
                     .Input(FakeInput(dtype))
                     .Attr("T", dtype)
                     .Attr("resize_align_corners", false)
                     .Attr("mode", "REFLECT")
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("padding", "SAME")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());
    const int depth = 1;
    const int image_width = 4;
    const int image_height = 3;
    const int image_batch_count = 1;
    // The image matrix is:
    // |  1 |  2 |  3 |  4 |
    // |  5 |  6 |  7 |  8 |
    // |  9 | 10 | 11 | 12 |
    Tensor image(dtype, {image_batch_count, image_height, image_width, depth});
    test::FillValues<T>(&image, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

    // The filter matrix is:
    // | 1 | 4 | 7 |
    // | 2 | 5 | 8 |
    // | 3 | 6 | 9 |
    const int filter_size = 3;
    const int filter_count = 1;
    Tensor filter(dtype, {filter_size, filter_size, depth, filter_count});
    test::FillValues<T>(&filter, {1, 4, 7, 2, 5, 8, 3, 6, 9});

    const int resized_width = image_width;
    const int resized_height = image_height;

    const int top_padding = 0;
    const int bottom_padding = 0;
    const int left_padding = 0;
    const int right_padding = 0;

    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<int32>(TensorShape({2}), {resized_height, resized_width});
    AddInputFromArray<int32>(
        TensorShape({4, 2}),
        {0, 0, top_padding, bottom_padding, left_padding, right_padding, 0, 0});
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    TF_ASSERT_OK(RunOpKernel());

    // We're sliding the 3x3 filter across the 3x4 image, with accesses outside
    // the input set to zero because we're using the 'SAME' padding mode.
    // The calculations behind the expected output are:
    // (1*0)+(4*0)+(7*0)+(2*0)+(5*1)+(8*2)+(3*0)+(6*5)+(9*6)=105
    // (1*0)+(4*0)+(7*0)+(2*1)+(5*2)+(8*3)+(3*5)+(6*6)+(9*7)=150
    // (1*0)+(4*0)+(7*0)+(2*2)+(5*3)+(8*4)+(3*6)+(6*7)+(9*8)=183
    // (1*0)+(4*0)+(7*0)+(2*3)+(5*4)+(8*0)+(3*7)+(6*8)+(9*0)=95
    // (1*0)+(4*1)+(7*2)+(2*0)+(5*5)+(8*6)+(3*0)+(6*9)+(9*10)=235
    // (1*1)+(4*2)+(7*3)+(2*5)+(5*6)+(8*7)+(3*9)+(6*10)+(9*11)=312
    // (1*2)+(4*3)+(7*4)+(2*6)+(5*7)+(8*8)+(3*10)+(6*11)+(9*12)=357
    // (1*3)+(4*4)+(7*0)+(2*7)+(5*8)+(8*0)+(3*11)+(6*12)+(9*0)=178
    // (1*0)+(4*5)+(7*6)+(2*0)+(5*9)+(8*10)+(3*0)+(6*0)+(9*0)=187
    // (1*5)+(4*6)+(7*7)+(2*9)+(5*10)+(8*11)+(3*0)+(6*0)+(9*0)=234
    // (1*6)+(4*7)+(7*8)+(2*10)+(5*11)+(8*12)+(3*0)+(6*0)+(9*0)=261
    // (1*7)+(4*11)+(7*0)+(2*8)+(5*12)+(8*0)+(3*0)+(6*0)+(9*0)=121
    // This means we should end up with this matrix:
    // |  105  |  150  |  183  |   95  |
    // |  235  |  312  |  357  |  178  |
    // |  187  |  234  |  261  |  121  |
    const int expected_width = image_width;
    const int expected_height = image_height * filter_count;
    Tensor expected(dtype, TensorShape({image_batch_count, expected_height,
                                        expected_width, filter_count}));
    test::FillValues<T>(
        &expected, {105, 150, 183, 95, 235, 312, 357, 178, 187, 234, 261, 121});
    const Tensor& output = *GetOutput(0);
    test::ExpectTensorNear<T>(expected, output, 1e-5);
  }

  template <typename T>
  void CompareFusedAndSeparate(int input_width, int input_height,
                               int input_depth, int resize_width,
                               int resize_height, int y_padding, int x_padding,
                               int filter_size, int filter_count,
                               bool resize_align_corners,
                               const string& pad_mode, int stride,
                               const string& padding, DataType dtype) {
    auto root = tensorflow::Scope::NewRootScope();
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    Tensor input_data(DT_FLOAT,
                      TensorShape({1, input_height, input_width, input_depth}));
    test::FillIota<float>(&input_data, 1.0f);
    Output input =
        Const(root.WithOpName("input"), Input::Initializer(input_data));
    Output casted_input = Cast(root.WithOpName("casted_input"), input, dtype);

    Tensor filter_data(DT_FLOAT, TensorShape({filter_size, filter_size,
                                              input_depth, filter_count}));
    test::FillIota<float>(&filter_data, 1.0f);
    Output filter =
        Const(root.WithOpName("filter"), Input::Initializer(filter_data));
    Output casted_filter =
        Cast(root.WithOpName("casted_filter"), filter, dtype);

    Output resize_size =
        Const(root.WithOpName("resize_size"), {resize_height, resize_width});
    Output resize =
        ResizeBilinear(root.WithOpName("resize"), input, resize_size,
                       ResizeBilinear::AlignCorners(resize_align_corners));
    // Bilinear resize only output float, cast it to dtype to match the input.
    Output casted_resize = Cast(root.WithOpName("cast"), resize, dtype);
    Output paddings =
        Const(root.WithOpName("paddings"),
              {{0, 0}, {y_padding, y_padding}, {x_padding, x_padding}, {0, 0}});
    Output mirror_pad = MirrorPad(root.WithOpName("mirror_pad"), casted_resize,
                                  paddings, pad_mode);
    Output conv = Conv2D(root.WithOpName("conv"), mirror_pad, casted_filter,
                         {1, stride, stride, 1}, padding);

    Output fused_conv = FusedResizeAndPadConv2D(
        root.WithOpName("fused_conv"), casted_input, resize_size, paddings,
        casted_filter, pad_mode, {1, stride, stride, 1}, padding,
        FusedResizeAndPadConv2D::ResizeAlignCorners(resize_align_corners));

    tensorflow::GraphDef graph;
    TF_ASSERT_OK(root.ToGraphDef(&graph));

    std::unique_ptr<tensorflow::Session> session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(session->Create(graph));

    std::vector<Tensor> unfused_tensors;
    TF_ASSERT_OK(session->Run({}, {"conv"}, {}, &unfused_tensors));

    std::vector<Tensor> fused_tensors;
    TF_ASSERT_OK(session->Run({}, {"fused_conv"}, {}, &fused_tensors));

    test::ExpectClose(unfused_tensors[0], fused_tensors[0]);
  }

  template <typename T>
  void CompareFusedPadOnlyAndSeparate(int input_width, int input_height,
                                      int input_depth, int y_padding,
                                      int x_padding, int filter_size,
                                      int filter_count, const string& pad_mode,
                                      int stride, const string& padding,
                                      DataType dtype) {
    auto root = tensorflow::Scope::NewRootScope();
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    Tensor input_data(DT_FLOAT,
                      TensorShape({1, input_height, input_width, input_depth}));
    test::FillIota<float>(&input_data, 1.0f);
    Output input =
        Const(root.WithOpName("input"), Input::Initializer(input_data));
    Output casted_input = Cast(root.WithOpName("casted_input"), input, dtype);

    Tensor filter_data(DT_FLOAT, TensorShape({filter_size, filter_size,
                                              input_depth, filter_count}));
    test::FillIota<float>(&filter_data, 1.0f);
    Output filter =
        Const(root.WithOpName("filter"), Input::Initializer(filter_data));
    Output casted_filter =
        Cast(root.WithOpName("casted_filter"), filter, dtype);

    Output paddings =
        Const(root.WithOpName("paddings"),
              {{0, 0}, {y_padding, y_padding}, {x_padding, x_padding}, {0, 0}});
    Output mirror_pad = MirrorPad(root.WithOpName("mirror_pad"), casted_input,
                                  paddings, pad_mode);
    Output conv = Conv2D(root.WithOpName("conv"), mirror_pad, casted_filter,
                         {1, stride, stride, 1}, padding);

    Output fused_conv = FusedPadConv2D(
        root.WithOpName("fused_conv"), casted_input, paddings, casted_filter,
        pad_mode, {1, stride, stride, 1}, padding);

    tensorflow::GraphDef graph;
    TF_ASSERT_OK(root.ToGraphDef(&graph));

    std::unique_ptr<tensorflow::Session> session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(session->Create(graph));

    std::vector<Tensor> unfused_tensors;
    TF_ASSERT_OK(session->Run({}, {"conv"}, {}, &unfused_tensors));

    std::vector<Tensor> fused_tensors;
    TF_ASSERT_OK(session->Run({}, {"fused_conv"}, {}, &fused_tensors));

    test::ExpectClose(unfused_tensors[0], fused_tensors[0]);
  }
};

TEST_F(FusedResizePadConvOpTest, HandwrittenConvHalf) {
  HandwrittenConv<Eigen::half>(DT_HALF);
}

TEST_F(FusedResizePadConvOpTest, HandwrittenConvFloat) {
  HandwrittenConv<float>(DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, HandwrittenConvDouble) {
  HandwrittenConv<double>(DT_DOUBLE);
}

TEST_F(FusedResizePadConvOpTest, IdentityComparativeHalf) {
  CompareFusedAndSeparate<Eigen::half>(10, 10, 1, 10, 10, 0, 0, 1, 1, false,
                                       "REFLECT", 1, "SAME", DT_HALF);
}

TEST_F(FusedResizePadConvOpTest, IdentityComparativeFloat) {
  CompareFusedAndSeparate<float>(10, 10, 1, 10, 10, 0, 0, 1, 1, false,
                                 "REFLECT", 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, IdentityComparativeDouble) {
  CompareFusedAndSeparate<double>(10, 10, 1, 10, 10, 0, 0, 1, 1, false,
                                  "REFLECT", 1, "SAME", DT_DOUBLE);
}

TEST_F(FusedResizePadConvOpTest, ConvOnlyComparative) {
  CompareFusedAndSeparate<float>(10, 10, 3, 10, 10, 0, 0, 4, 4, false,
                                 "REFLECT", 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeOnlyComparative) {
  CompareFusedAndSeparate<float>(10, 10, 1, 20, 20, 0, 0, 1, 1, false,
                                 "REFLECT", 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAndConvComparative) {
  CompareFusedAndSeparate<float>(2, 2, 4, 4, 2, 0, 0, 2, 2, false, "REFLECT", 1,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAlignAndConvComparative) {
  CompareFusedAndSeparate<float>(2, 2, 4, 4, 2, 0, 0, 2, 2, true, "REFLECT", 1,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAndConvStridedComparative) {
  CompareFusedAndSeparate<float>(2, 2, 4, 4, 2, 0, 0, 2, 2, false, "REFLECT", 2,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAlignAndConvValidComparative) {
  CompareFusedAndSeparate<float>(2, 2, 4, 4, 2, 0, 0, 2, 2, true, "REFLECT", 1,
                                 "VALID", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, PadOnlyComparative) {
  CompareFusedAndSeparate<float>(4, 4, 1, 4, 4, 2, 2, 1, 1, false, "REFLECT", 1,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, PadOnlyWithChannelsComparative) {
  CompareFusedAndSeparate<float>(4, 4, 3, 4, 4, 2, 2, 1, 1, false, "REFLECT", 1,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAndPadComparative) {
  CompareFusedAndSeparate<float>(4, 4, 1, 6, 6, 2, 2, 1, 1, false, "REFLECT", 1,
                                 "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, PadOnlySymmetricComparative) {
  CompareFusedAndSeparate<float>(4, 4, 1, 4, 4, 2, 2, 1, 1, false, "SYMMETRIC",
                                 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAndPadSymmetricComparative) {
  CompareFusedAndSeparate<float>(4, 4, 3, 6, 6, 2, 2, 1, 1, false, "SYMMETRIC",
                                 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, ResizeAndPadSymmetricComparativeLarge) {
  CompareFusedAndSeparate<float>(1000, 1000, 3, 1006, 1006, 2, 2, 1, 1, false,
                                 "SYMMETRIC", 1, "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, NoResizeIdentityComparativeHalf) {
  CompareFusedPadOnlyAndSeparate<Eigen::half>(10, 10, 1, 0, 0, 1, 1, "REFLECT",
                                              1, "SAME", DT_HALF);
}

TEST_F(FusedResizePadConvOpTest, NoResizeIdentityComparativeFloat) {
  CompareFusedPadOnlyAndSeparate<float>(10, 10, 1, 0, 0, 1, 1, "REFLECT", 1,
                                        "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, NoResizeIdentityComparativeDouble) {
  CompareFusedPadOnlyAndSeparate<double>(10, 10, 1, 0, 0, 1, 1, "REFLECT", 1,
                                         "SAME", DT_DOUBLE);
}

TEST_F(FusedResizePadConvOpTest, NoResizeConvOnlyComparative) {
  CompareFusedPadOnlyAndSeparate<float>(10, 10, 3, 0, 0, 4, 4, "REFLECT", 1,
                                        "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, NoResizePadOnlyComparative) {
  CompareFusedPadOnlyAndSeparate<float>(4, 4, 1, 2, 2, 1, 1, "REFLECT", 1,
                                        "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, NoResizePadOnlyWithChannelsComparative) {
  CompareFusedPadOnlyAndSeparate<float>(4, 4, 3, 2, 2, 1, 1, "REFLECT", 1,
                                        "SAME", DT_FLOAT);
}

TEST_F(FusedResizePadConvOpTest, NoResizePadOnlySymmetricComparative) {
  CompareFusedPadOnlyAndSeparate<float>(4, 4, 1, 2, 2, 1, 1, "SYMMETRIC", 1,
                                        "SAME", DT_FLOAT);
}

class ConvOpTest : public OpsTestBase {
 protected:
  void HandwrittenConv() {
    const int stride = 1;
    TF_EXPECT_OK(NodeDefBuilder("conv_op", "Conv2D")
                     .Input(FakeInput(DT_FLOAT))
                     .Input(FakeInput(DT_FLOAT))
                     .Attr("T", DT_FLOAT)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("padding", "SAME")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());
    const int depth = 1;
    const int image_width = 4;
    const int image_height = 3;
    const int image_batch_count = 1;
    // The image matrix is:
    // |  1 |  2 |  3 |  4 |
    // |  5 |  6 |  7 |  8 |
    // |  9 | 10 | 11 | 12 |
    Tensor image(DT_FLOAT,
                 {image_batch_count, image_height, image_width, depth});
    test::FillValues<float>(&image, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

    // The filter matrix is:
    // | 1 | 4 | 7 |
    // | 2 | 5 | 8 |
    // | 3 | 6 | 9 |
    const int filter_size = 3;
    const int filter_count = 1;
    Tensor filter(DT_FLOAT, {filter_size, filter_size, depth, filter_count});
    test::FillValues<float>(&filter, {1, 4, 7, 2, 5, 8, 3, 6, 9});

    AddInputFromArray<float>(image.shape(), image.flat<float>());
    AddInputFromArray<float>(filter.shape(), filter.flat<float>());
    TF_ASSERT_OK(RunOpKernel());

    // We're sliding the 3x3 filter across the 3x4 image, with accesses outside
    // the input set to zero because we're using the 'SAME' padding mode.
    // The calculations behind the expected output are:
    // (1*0)+(4*0)+(7*0)+(2*0)+(5*1)+(8*2)+(3*0)+(6*5)+(9*6)=105
    // (1*0)+(4*0)+(7*0)+(2*1)+(5*2)+(8*3)+(3*5)+(6*6)+(9*7)=150
    // (1*0)+(4*0)+(7*0)+(2*2)+(5*3)+(8*4)+(3*6)+(6*7)+(9*8)=183
    // (1*0)+(4*0)+(7*0)+(2*3)+(5*4)+(8*0)+(3*7)+(6*8)+(9*0)=95
    // (1*0)+(4*1)+(7*2)+(2*0)+(5*5)+(8*6)+(3*0)+(6*9)+(9*10)=235
    // (1*1)+(4*2)+(7*3)+(2*5)+(5*6)+(8*7)+(3*9)+(6*10)+(9*11)=312
    // (1*2)+(4*3)+(7*4)+(2*6)+(5*7)+(8*8)+(3*10)+(6*11)+(9*12)=357
    // (1*3)+(4*4)+(7*0)+(2*7)+(5*8)+(8*0)+(3*11)+(6*12)+(9*0)=178
    // (1*0)+(4*5)+(7*6)+(2*0)+(5*9)+(8*10)+(3*0)+(6*0)+(9*0)=187
    // (1*5)+(4*6)+(7*7)+(2*9)+(5*10)+(8*11)+(3*0)+(6*0)+(9*0)=234
    // (1*6)+(4*7)+(7*8)+(2*10)+(5*11)+(8*12)+(3*0)+(6*0)+(9*0)=261
    // (1*7)+(4*8)+(7*0)+(2*11)+(5*12)+(8*0)+(3*0)+(6*0)+(9*0)=121
    // This means we should end up with this matrix:
    // |  105  |  150  |  183  |   95  |
    // |  235  |  312  |  357  |  178  |
    // |  187  |  234  |  261  |  121  |
    const int expected_width = image_width;
    const int expected_height = image_height * filter_count;
    Tensor expected(DT_FLOAT, TensorShape({image_batch_count, expected_height,
                                           expected_width, filter_count}));
    test::FillValues<float>(
        &expected, {105, 150, 183, 95, 235, 312, 357, 178, 187, 234, 261, 121});
    const Tensor& output = *GetOutput(0);
    test::ExpectTensorNear<float>(expected, output, 1e-5);
  }

  void AnisotropicStrides() {
    const int stride_width = 3;
    const int stride_height = 1;
    TF_EXPECT_OK(NodeDefBuilder("conv_op", "Conv2D")
                     .Input(FakeInput(DT_FLOAT))
                     .Input(FakeInput(DT_FLOAT))
                     .Attr("T", DT_FLOAT)
                     .Attr("strides", {1, stride_height, stride_width, 1})
                     .Attr("padding", "VALID")
                     .Finalize(node_def()));
    TF_EXPECT_OK(InitOp());
    const int depth = 1;
    const int image_width = 6;
    const int image_height = 3;
    const int image_batch_count = 1;
    Tensor image(DT_FLOAT,
                 {image_batch_count, image_height, image_width, depth});
    test::FillValues<float>(&image, {
                                        3, 2, 1, -1, -2, -3,  //
                                        4, 3, 2, -2, -3, -4,  //
                                        5, 4, 3, -3, -4, -5,  //
                                    });
    const int filter_size = 2;
    const int filter_count = 1;
    Tensor filter(DT_FLOAT, {filter_size, filter_size, depth, filter_count});
    test::FillValues<float>(&filter, {
                                         1, 2,  //
                                         3, 4,  //
                                     });

    AddInputFromArray<float>(image.shape(), image.flat<float>());
    AddInputFromArray<float>(filter.shape(), filter.flat<float>());
    TF_ASSERT_OK(RunOpKernel());

    const int expected_width = 2;
    const int expected_height = 2;
    Tensor expected(DT_FLOAT, TensorShape({image_batch_count, expected_height,
                                           expected_width, filter_count}));
    test::FillValues<float>(&expected, {31, -23, 41, -33});
    const Tensor& output = *GetOutput(0);
    test::ExpectTensorNear<float>(expected, output, 1e-5);
  }
};

TEST_F(ConvOpTest, HandwrittenConv) { HandwrittenConv(); }

TEST_F(ConvOpTest, AnisotropicStride) { AnisotropicStrides(); }

class FusedConv2DOpTest : public OpsTestBase {
 protected:
  static constexpr int kDepth = 3;
  static constexpr int kImageWidth = 32;
  static constexpr int kImageHeight = 32;
  static constexpr int kImageBatchCount = 8;

  using GraphRunner =
      std::function<void(const Tensor& input_data, const Tensor& filter_data,
                         const Tensor& bias_data, Tensor* out)>;

  // Runs a Tensorflow graph defined by the root scope, and fetches the result
  // of 'fetch' node into the output Tensor.
  void RunAndFetch(const tensorflow::Scope& root, const string& fetch,
                   Tensor* output) {
    tensorflow::GraphDef graph;
    TF_ASSERT_OK(root.ToGraphDef(&graph));

    std::unique_ptr<tensorflow::Session> session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(session->Create(graph));

    std::vector<Tensor> unfused_tensors;
    TF_ASSERT_OK(session->Run({}, {fetch}, {}, &unfused_tensors));

    *output = unfused_tensors[0];
  }

  void RunConv2DOp(const Tensor& input_data, const Tensor& filter_data,
                   const Tensor& bias_data, Tensor* output, int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    auto conv = ops::Conv2D(
        root.WithOpName("conv"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "SAME");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    RunAndFetch(root, "with_bias", output);
  }

  void RunConv2DWithReluOp(const Tensor& input_data, const Tensor& filter_data,
                           const Tensor& bias_data, Tensor* output,
                           int stride = 1) {
    auto root = tensorflow::Scope::NewRootScope();

    auto conv = ops::Conv2D(
        root.WithOpName("conv"),
        ops::Const(root.WithOpName("input"), Input::Initializer(input_data)),
        ops::Const(root.WithOpName("filter"), Input::Initializer(filter_data)),
        {1, stride, stride, 1}, "SAME");

    auto with_bias = ops::BiasAdd(
        root.WithOpName("with_bias"), conv,
        ops::Const(root.WithOpName("bias"), Input::Initializer(bias_data)));

    auto with_relu = ops::Relu(root.WithOpName("with_relu"), with_bias);

    RunAndFetch(root, "with_relu", output);
  }

  template <typename T>
  void RunFusedConv2DOp(const Tensor& image, const Tensor& filter,
                        const Tensor& bias,
                        const std::vector<string>& fused_ops, Tensor* output,
                        int stride = 1) {
    DataType dtype = DataTypeToEnum<T>::v();

    TF_EXPECT_OK(NodeDefBuilder("fused_conv_op", "_FusedConv2D")
                     .Input(FakeInput(dtype))
                     .Input(FakeInput(dtype))
                     .Attr("num_args", 1)
                     .Input(FakeInput(dtype))
                     .Attr("T", dtype)
                     .Attr("strides", {1, stride, stride, 1})
                     .Attr("padding", "SAME")
                     .Attr("fused_ops", fused_ops)
                     .Finalize(node_def()));

    TF_EXPECT_OK(InitOp());

    AddInputFromArray<T>(image.shape(), image.flat<T>());
    AddInputFromArray<T>(filter.shape(), filter.flat<T>());
    AddInputFromArray<T>(bias.shape(), bias.flat<T>());
    TF_ASSERT_OK(RunOpKernel());

    *output = *GetOutput(0);
  }

  template <typename T>
  void VerifyTensorsNear(int depth, int image_width, int image_height,
                         int image_batch_count, int filter_size,
                         int filter_count, const GraphRunner& run_default,
                         const GraphRunner& run_fused) {
    DataType dtype = DataTypeToEnum<T>::v();
    Tensor image(dtype, {image_batch_count, image_height, image_width, depth});
    image.flat<T>() = image.flat<T>().setRandom();

    Tensor filter(dtype, {filter_size, filter_size, depth, filter_count});
    filter.flat<T>() = filter.flat<T>().setRandom();

    const int bias_size = filter_count;
    Tensor bias(dtype, {bias_size});
    bias.flat<T>() = bias.flat<T>().setRandom();

    Tensor conv_2d;
    Tensor fused_conv_2d;

    run_default(image, filter, bias, &conv_2d);
    run_fused(image, filter, bias, &fused_conv_2d);

    ASSERT_EQ(conv_2d.dtype(), fused_conv_2d.dtype());
    ASSERT_EQ(conv_2d.shape(), fused_conv_2d.shape());

    test::ExpectTensorNear<T>(conv_2d, fused_conv_2d, 1e-5);
  }

  // Verifies that computing Conv2D+BiasAdd in a graph is identical to
  // FusedConv2D.
  template <typename T>
  void VerifyConv2DWithBias(int depth, int image_width, int image_height,
                            int image_batch_count, int filter_size,
                            int filter_count) {
    const GraphRunner run_default =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunConv2DOp(input_data, filter_data, bias_data, out);
        };

    const GraphRunner run_fused = [this](const Tensor& input_data,
                                         const Tensor& filter_data,
                                         const Tensor& bias_data, Tensor* out) {
      RunFusedConv2DOp<T>(input_data, filter_data, bias_data, {"BiasAdd"}, out);
    };

    VerifyTensorsNear<T>(depth, image_width, image_height, image_batch_count,
                         filter_size, filter_count, run_default, run_fused);
  }

  // Verifies that computing Conv2D+BiasAdd+Relu in a graph is identical to
  // FusedConv2D.
  template <typename T>
  void VerifyConv2DWithBiasAndRelu(int depth, int image_width, int image_height,
                                   int image_batch_count, int filter_size,
                                   int filter_count) {
    const GraphRunner run_default =
        [this](const Tensor& input_data, const Tensor& filter_data,
               const Tensor& bias_data, Tensor* out) {
          RunConv2DWithReluOp(input_data, filter_data, bias_data, out);
        };

    const GraphRunner run_fused = [this](const Tensor& input_data,
                                         const Tensor& filter_data,
                                         const Tensor& bias_data, Tensor* out) {
      RunFusedConv2DOp<T>(input_data, filter_data, bias_data,
                          {"BiasAdd", "Relu"}, out);
    };

    VerifyTensorsNear<T>(depth, image_width, image_height, image_batch_count,
                         filter_size, filter_count, run_default, run_fused);
  }
};

#define FUSED_CONV2D_TESTS(dtype, name)                                       \
  TEST_F(FusedConv2DOpTest, Conv2DWithBiasAddOneByOneConvolution##name) {     \
    const int filter_size = 1;                                                \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBias<dtype>(kDepth, kImageWidth, kImageHeight,            \
                                kImageBatchCount, filter_size, filter_count); \
  }                                                                           \
                                                                              \
  TEST_F(FusedConv2DOpTest, Conv2DWithBiasAddImageSizeConvolution##name) {    \
    const int filter_size = 32;                                               \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBias<dtype>(kDepth, kImageWidth, kImageHeight,            \
                                kImageBatchCount, filter_size, filter_count); \
  }                                                                           \
                                                                              \
  TEST_F(FusedConv2DOpTest, Conv2DWithBiasAddSpatialConvolution##name) {      \
    const int filter_size = 3;                                                \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBias<dtype>(kDepth, kImageWidth, kImageHeight,            \
                                kImageBatchCount, filter_size, filter_count); \
  }                                                                           \
                                                                              \
  TEST_F(FusedConv2DOpTest,                                                   \
         Conv2DWithBiasAddAndReluOneByOneConvolution##name) {                 \
    const int filter_size = 1;                                                \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBiasAndRelu<dtype>(kDepth, kImageWidth, kImageHeight,     \
                                       kImageBatchCount, filter_size,         \
                                       filter_count);                         \
  }                                                                           \
                                                                              \
  TEST_F(FusedConv2DOpTest,                                                   \
         Conv2DWithBiasAddAndReluImageSizeConvolution##name) {                \
    const int filter_size = 32;                                               \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBiasAndRelu<dtype>(kDepth, kImageWidth, kImageHeight,     \
                                       kImageBatchCount, filter_size,         \
                                       filter_count);                         \
  }                                                                           \
                                                                              \
  TEST_F(FusedConv2DOpTest,                                                   \
         Conv2DWithBiasAddAndReluSpatialConvolution##name) {                  \
    const int filter_size = 3;                                                \
    const int filter_count = 12;                                              \
                                                                              \
    VerifyConv2DWithBiasAndRelu<dtype>(kDepth, kImageWidth, kImageHeight,     \
                                       kImageBatchCount, filter_size,         \
                                       filter_count);                         \
  }

FUSED_CONV2D_TESTS(float, F);
FUSED_CONV2D_TESTS(double, D);

#undef FUSED_CONV2D_TESTS

////////////////////////////////////////////////////////////////////////////////
// Performance benchmarks for the FusedConv2DWithBiasOp.                      //
////////////////////////////////////////////////////////////////////////////////

struct Conv2DGraph {
  Graph* graph;
  Node* conv2d;
};

struct Conv2DWithBiasGraph {
  Graph* graph;
  Node* conv2d;
  Node* bias;
};

struct Conv2DWithBiasAndReluGraph {
  Graph* graph;
  Node* conv2d;
  Node* bias;
  Node* relu;
};

static Tensor MakeRandomTensor(const TensorShape& shape) {
  Tensor tensor(DT_FLOAT, TensorShape(shape));
  tensor.flat<float>() = tensor.flat<float>().setRandom();
  return tensor;
}

// Creates a simple Tensorflow graph with single Conv2D node.
static Conv2DGraph Conv2D(int batch, int height, int width, int in_depth,
                          int filter_w, int filter_h, int out_depth) {
  Graph* graph = new Graph(OpRegistry::Global());

  Tensor images_t = MakeRandomTensor({batch, height, width, in_depth});
  Tensor filter_t = MakeRandomTensor({filter_w, filter_h, in_depth, out_depth});

  Node* images = test::graph::Constant(graph, images_t, "images");
  Node* filter = test::graph::Constant(graph, filter_t, "filter");

  Node* conv2d;
  TF_CHECK_OK(NodeBuilder(graph->NewName("conv"), "Conv2D")
                  .Input(images)
                  .Input(filter)
                  .Attr("T", DT_FLOAT)
                  .Attr("strides", {1, 1, 1, 1})
                  .Attr("padding", "SAME")
                  .Finalize(graph, &conv2d));

  return {graph, conv2d};
}

// Creates a Tensorflow graph with a Conv2D node followed by Relu.
static Conv2DWithBiasGraph Conv2DWithBias(int batch, int height, int width,
                                          int in_depth, int filter_w,
                                          int filter_h, int out_depth) {
  Conv2DGraph conv_graph =
      Conv2D(batch, height, width, in_depth, filter_w, filter_h, out_depth);

  Graph* graph = conv_graph.graph;
  Node* conv2d = conv_graph.conv2d;

  Tensor bias_t = MakeRandomTensor({out_depth});
  Node* bias = test::graph::Constant(graph, bias_t, "bias");

  Node* out;
  TF_CHECK_OK(NodeBuilder(graph->NewName("bias"), "BiasAdd")
                  .Input(conv2d)
                  .Input(bias)
                  .Attr("T", DT_FLOAT)
                  .Attr("data_format", "NHWC")
                  .Finalize(graph, &out));

  return {graph, conv2d, out};
}

// Creates a Tensorflow graph with a Conv2D node followed by BiasAdd and Relu.
static Conv2DWithBiasAndReluGraph Conv2DWithBiasAndRelu(int batch, int height,
                                                        int width, int in_depth,
                                                        int filter_w,
                                                        int filter_h,
                                                        int out_depth) {
  Conv2DWithBiasGraph conv_graph = Conv2DWithBias(
      batch, height, width, in_depth, filter_w, filter_h, out_depth);

  Graph* graph = conv_graph.graph;
  Node* conv2d = conv_graph.conv2d;
  Node* bias = conv_graph.bias;

  Node* relu;
  TF_CHECK_OK(NodeBuilder(graph->NewName("relu"), "Relu")
                  .Input(bias)
                  .Attr("T", DT_FLOAT)
                  .Finalize(graph, &relu));

  return {graph, conv2d, bias, relu};
}

// Creates a tensorflow graph with a single FusedConv2D node and fuses into it
// additional computations (e.g. BiasAdd or Relu).
static Graph* FusedConv2D(int batch, int height, int width, int in_depth,
                          int filter_w, int filter_h, int out_depth,
                          const std::vector<string>& fused_ops = {}) {
  Graph* graph = new Graph(OpRegistry::Global());

  Tensor images_t = MakeRandomTensor({batch, height, width, in_depth});
  Tensor filter_t = MakeRandomTensor({filter_w, filter_h, in_depth, out_depth});
  Tensor bias_t = MakeRandomTensor({out_depth});

  Node* images = test::graph::Constant(graph, images_t, "images");
  Node* filter = test::graph::Constant(graph, filter_t, "filter");
  Node* bias = test::graph::Constant(graph, bias_t, "bias");

  std::vector<NodeBuilder::NodeOut> args = {bias};

  Node* conv;
  TF_CHECK_OK(NodeBuilder(graph->NewName("conv"), "_FusedConv2D")
                  .Input(images)
                  .Input(filter)
                  .Attr("num_args", 1)
                  .Input(args)
                  .Attr("T", DT_FLOAT)
                  .Attr("strides", {1, 1, 1, 1})
                  .Attr("padding", "SAME")
                  .Attr("fused_ops", fused_ops)
                  .Finalize(graph, &conv));

  return graph;
}

#define BM_SETUP(N, H, W, C, type, LABEL, NAME)                               \
  testing::ItemsProcessed(static_cast<int64>(iters) * (N) * (H) * (W) * (C)); \
  testing::SetLabel(LABEL);

#define BM_NAME(name, type, N, H, W, C, FW, FH, FC) \
  name##_##type##_##N##_##H##_##W##_##C##_##FW##_##FH##_##FC

#define BM_Conv2D(N, H, W, C, FW, FH, FC, type, LABEL)                       \
  static void BM_NAME(BM_Conv2D, type, N, H, W, C, FW, FH, FC)(int iters) {  \
    BM_SETUP(N, H, W, C, type, LABEL, Conv2D);                               \
    test::Benchmark(#type, Conv2D(N, H, W, C, FW, FH, FC).graph).Run(iters); \
  }                                                                          \
  BENCHMARK(BM_NAME(BM_Conv2D, type, N, H, W, C, FW, FH, FC));

#define BM_Conv2DWithBias(N, H, W, C, FW, FH, FC, type, LABEL)           \
  static void BM_NAME(BM_Conv2DWithBias, type, N, H, W, C, FW, FH,       \
                      FC)(int iters) {                                   \
    BM_SETUP(N, H, W, C, type, LABEL, Conv2D);                           \
    test::Benchmark(#type, Conv2DWithBias(N, H, W, C, FW, FH, FC).graph) \
        .Run(iters);                                                     \
  }                                                                      \
  BENCHMARK(BM_NAME(BM_Conv2DWithBias, type, N, H, W, C, FW, FH, FC));

#define BM_Conv2DWithBiasAndRelu(N, H, W, C, FW, FH, FC, type, LABEL)     \
  static void BM_NAME(BM_Conv2DWithBiasAndRelu, type, N, H, W, C, FW, FH, \
                      FC)(int iters) {                                    \
    BM_SETUP(N, H, W, C, type, LABEL, Conv2D);                            \
    test::Benchmark(#type,                                                \
                    Conv2DWithBiasAndRelu(N, H, W, C, FW, FH, FC).graph)  \
        .Run(iters);                                                      \
  }                                                                       \
  BENCHMARK(BM_NAME(BM_Conv2DWithBiasAndRelu, type, N, H, W, C, FW, FH, FC));

#define BM_FusedConv2D(N, H, W, C, FW, FH, FC, type, LABEL)                  \
  static void BM_NAME(BM_FusedConv2D, type, N, H, W, C, FW, FH,              \
                      FC)(int iters) {                                       \
    BM_SETUP(N, H, W, C, type, LABEL, Conv2D);                               \
    test::Benchmark(#type, FusedConv2D(N, H, W, C, FW, FH, FC, {"BiasAdd"})) \
        .Run(iters);                                                         \
  }                                                                          \
  BENCHMARK(BM_NAME(BM_FusedConv2D, type, N, H, W, C, FW, FH, FC));

#define BM_FusedConv2DAndRelu(N, H, W, C, FW, FH, FC, type, LABEL)            \
  static void BM_NAME(BM_FusedConv2DAndRelu, type, N, H, W, C, FW, FH,        \
                      FC)(int iters) {                                        \
    BM_SETUP(N, H, W, C, type, LABEL, Conv2D);                                \
    test::Benchmark(#type,                                                    \
                    FusedConv2D(N, H, W, C, FW, FH, FC, {"BiasAdd", "Relu"})) \
        .Run(iters);                                                          \
  }                                                                           \
  BENCHMARK(BM_NAME(BM_FusedConv2DAndRelu, type, N, H, W, C, FW, FH, FC));

// Pixel CNN convolutions.

// 1x1 Convolution: MatMulFunctor

BM_Conv2D(8, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 8");
BM_Conv2D(16, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 16");
BM_Conv2D(32, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 32");

BM_Conv2DWithBias(8, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 8");
BM_Conv2DWithBias(16, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 16");
BM_Conv2DWithBias(32, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 32");

BM_Conv2DWithBiasAndRelu(8, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 8");
BM_Conv2DWithBiasAndRelu(16, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 16");
BM_Conv2DWithBiasAndRelu(32, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 32");

BM_FusedConv2D(8, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 8");
BM_FusedConv2D(16, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 16");
BM_FusedConv2D(32, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 32");

BM_FusedConv2DAndRelu(8, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 8");
BM_FusedConv2DAndRelu(16, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 16");
BM_FusedConv2DAndRelu(32, 32, 32, 128, 1, 1, 1024, cpu, "1x1 /b 32");

// 3x3 Convolution: SpatialConvolution

BM_Conv2D(8, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 8");
BM_Conv2D(16, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 16");
BM_Conv2D(32, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 32");

BM_Conv2DWithBias(8, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 8");
BM_Conv2DWithBias(16, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 16");
BM_Conv2DWithBias(32, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 32");

BM_Conv2DWithBiasAndRelu(8, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 8");
BM_Conv2DWithBiasAndRelu(16, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 16");
BM_Conv2DWithBiasAndRelu(32, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 32");

BM_FusedConv2D(8, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 8");
BM_FusedConv2D(16, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 16");
BM_FusedConv2D(32, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 32");

BM_FusedConv2DAndRelu(8, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 8");
BM_FusedConv2DAndRelu(16, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 16");
BM_FusedConv2DAndRelu(32, 32, 32, 128, 3, 3, 1024, cpu, "3x3 /b 32");

}  // namespace tensorflow
