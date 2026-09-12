#include "RKNNEngine.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
void check(int code, const char *operation) {
    if (code < 0)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(code));
}
} // namespace
RKNNEngine::~RKNNEngine() {
    if (initialized_)
        rknn_destroy(ctx_);
}
void RKNNEngine::loadModel(const std::string &path) {
    if (initialized_)
        throw std::runtime_error("model already loaded");
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot read model: " + path);
    auto size = in.tellg();
    if (size <= 0 || size > 256 * 1024 * 1024)
        throw std::runtime_error("invalid model size");
    std::vector<char> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(bytes.data(), size))
        throw std::runtime_error("truncated model");
    check(rknn_init(&ctx_, bytes.data(), bytes.size(), 0, nullptr), "rknn_init");
    initialized_ = true;
    rknn_input_output_num io{};
    check(rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO");
    if (io.n_input != 1 || io.n_output != 3)
        throw std::runtime_error("requires YOLOv5 1 input/3 raw heads");
    rknn_tensor_attr input{};
    input.index = 0;
    check(rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)), "query input");
    bool input_shape =
        input.n_dims == 4 && ((input.fmt == RKNN_TENSOR_NHWC && input.dims[1] == 640 &&
                               input.dims[2] == 640 && input.dims[3] == 3) ||
                              (input.fmt == RKNN_TENSOR_NCHW && input.dims[1] == 3 &&
                               input.dims[2] == 640 && input.dims[3] == 640));
    if (!input_shape)
        throw std::runtime_error("requires 640x640 RGB input");
    for (int i = 0; i < 3; ++i) {
        auto &a = attrs_[i];
        a.index = i;
        check(rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)), "query output");
        std::cout << "[model] output " << i << " fmt=" << a.fmt << " type=" << a.type << " dims=";
        for (unsigned d = 0; d < a.n_dims; ++d)
            std::cout << a.dims[d] << ",";
        std::cout << " scale=" << a.scale << " zp=" << a.zp << std::endl;
        int grid = 80 >> i;
        if (a.n_dims != 4 || a.dims[0] != 1 || a.dims[1] != 255 || a.dims[2] != unsigned(grid) ||
            a.dims[3] != unsigned(grid) || a.fmt != RKNN_TENSOR_NCHW)
            throw std::runtime_error(
                "unsupported output layout; expected [1,255,H,W] heads at 80/40/20");
    }
}
InferenceResult RKNNEngine::infer(const cv::Mat &rgb, const Letterbox &box, float threshold,
                                  float nms, bool logits, bool native_outputs) {
    if (!initialized_ || rgb.type() != CV_8UC3 || rgb.rows != 640 || rgb.cols != 640 ||
        !rgb.isContinuous())
        throw std::runtime_error("invalid RKNN input");
    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = 640 * 640 * 3;
    input.buf = rgb.data;
    auto t0 = Clock::now();
    check(rknn_inputs_set(ctx_, 1, &input), "inputs_set");
    auto t1 = Clock::now();
    check(rknn_run(ctx_, nullptr), "rknn_run");
    auto t2 = Clock::now();
    std::array<rknn_output, 3> outputs{};
    bool quantized = native_outputs;
    for (const auto &a : attrs_)
        quantized = quantized && (a.type == RKNN_TENSOR_INT8 || a.type == RKNN_TENSOR_UINT8) &&
                    a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
    for (unsigned i = 0; i < 3; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = quantized ? 0 : 1;
    }
    check(rknn_outputs_get(ctx_, 3, outputs.data(), nullptr), "outputs_get");
    struct Guard {
        rknn_context ctx;
        rknn_output *p;
        ~Guard() {
            rknn_outputs_release(ctx, 3, p);
        }
    } guard{ctx_, outputs.data()};
    auto t3 = Clock::now();
    auto detections = Yolov5PostProcess().process(outputs.data(), box, threshold, nms, logits,
                                                  quantized ? attrs_.data() : nullptr);
    auto t4 = Clock::now();
    return {std::move(detections), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4)};
}
