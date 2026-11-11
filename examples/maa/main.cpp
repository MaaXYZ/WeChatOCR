#include <WeChatOCR/MaaAdapter.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

// This executable demonstrates the library API; the adapter does not launch it.
int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "Usage: wco_maa_example <absolute-runtime-dir> <absolute-ocr-path> <image.png>\n";
        return 2;
    }
    WcoOptions options {};
    options.struct_size = sizeof(options);
    options.abi_version = WCO_ABI_VERSION;
    options.runtime_dir = argv[1];
    options.ocr_path = argv[2];
    WcoEngine* raw_engine = nullptr;
    if (wco_create(&options, &raw_engine) != WCO_OK) {
        std::cerr << wco_last_error() << '\n';
        return 1;
    }
    const std::unique_ptr<WcoEngine, decltype(&wco_destroy)> engine(raw_engine, wco_destroy);
    const std::unique_ptr<MaaResource, decltype(&MaaResourceDestroy)> resource(MaaResourceCreate(), MaaResourceDestroy);
    const std::unique_ptr<MaaTasker, decltype(&MaaTaskerDestroy)> tasker(MaaTaskerCreate(), MaaTaskerDestroy);
    const std::unique_ptr<MaaImageBuffer, decltype(&MaaImageBufferDestroy)> image(MaaImageBufferCreate(), MaaImageBufferDestroy);
    if (!resource || !tasker || !image || !MaaTaskerBindResource(tasker.get(), resource.get())) {
        return 1;
    }

    std::ifstream stream(argv[3], std::ios::binary);
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(stream)), {});
    if (encoded.empty() || !MaaImageBufferSetEncoded(image.get(), encoded.data(), encoded.size())) {
        std::cerr << "Cannot read image\n";
        return 1;
    }
    WcoMaaRegistration* registration = nullptr;
    if (wco_maa_register(resource.get(), engine.get(), nullptr, &registration) != WCO_OK) {
        std::cerr << wco_maa_last_error() << '\n';
        return 1;
    }

    const auto id = MaaTaskerPostRecognition(tasker.get(), "Custom",
        R"({"custom_recognition":"WeChatOCR","custom_recognition_param":{"threshold":0,"timeout_ms":10000}})", image.get());
    const auto status = MaaTaskerWait(tasker.get(), id);
    const std::unique_ptr<MaaStringBuffer, decltype(&MaaStringBufferDestroy)>
        detail(MaaStringBufferCreate(), MaaStringBufferDestroy);
    MaaNodeId node_id = MaaInvalidId;
    MaaSize node_count = 1;
    MaaStatus task_status = MaaStatus_Invalid;
    MaaRecoId reco_id = MaaInvalidId;
    MaaBool hit = false;
    const auto has_detail = detail
        && MaaTaskerGetTaskDetail(tasker.get(), id, nullptr, &node_id, &node_count, &task_status)
        && node_count == 1
        && MaaTaskerGetNodeDetail(tasker.get(), node_id, nullptr, &reco_id, nullptr, nullptr)
        && MaaTaskerGetRecognitionDetail(tasker.get(), reco_id, nullptr, nullptr, &hit,
                                         nullptr, detail.get(), nullptr, nullptr);
    // MaaTaskerWait joins this recognition; normal applications must also stop
    // and join every other task before releasing this borrowed registration.
    const auto unregister_status = wco_maa_unregister(registration);
    std::cout << "Maa task status: " << status << "; hit: " << (hit ? "true" : "false") << '\n';
    if (has_detail) {
        std::cout << MaaStringBufferGet(detail.get()) << '\n';
    }
    return status == MaaStatus_Succeeded && has_detail && unregister_status == WCO_OK ? 0 : 1;
}
