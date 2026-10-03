# 原 baseline 无此端口；固定上游与其实际子模块，禁用 GPU/MKL 和独立 CLI。
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OpenNMT/CTranslate2
    REF 617405f4b050e994e829d527da6caa0e0030afe7
    SHA512 e37ad39437b4674547ff3dfff427aeafa8d3b5649dd6e44cd8b5ed893acf7ff53e1bf2f3070b0e2a69c7fbb23d9eb03eaccf51d816ca1dd0287394ecfb4cbd92
    # 解码逐步检查取消／截止，beam search 不再依赖仅支持贪心的 token 回调。
    PATCHES check-onednn-status.patch qualify-bit-cast.patch use-vcpkg-json.patch cooperative-stop.patch
)
vcpkg_from_github(
    OUT_SOURCE_PATH CPU_FEATURES_SOURCE
    REPO google/cpu_features
    REF 8a494eb1e158ec2050e5f699a504fbc9b896a43b
    SHA512 036a63bb0255491bdadcb9949abfb5b357465a9161c31dc0d82f27dc2168f9836baa62beee639da25efdf8a5d88de5110621b4656b005d32a7390478120cc48f
)
vcpkg_from_github(
    OUT_SOURCE_PATH SPDLOG_SOURCE
    REPO gabime/spdlog
    REF 76fb40d95455f249bd70824ecfcae7a8f0930fa3
    SHA512 a9e236144af0b4eaabac1fad358d2188124711b036435deb1e69acb258cac096a8f72eec4508784d8c63f4f526ea29991206277415072df501be5ac52c97fba2
)
file(COPY "${CPU_FEATURES_SOURCE}/" DESTINATION "${SOURCE_PATH}/third_party/cpu_features")
file(COPY "${SPDLOG_SOURCE}/" DESTINATION "${SOURCE_PATH}/third_party/spdlog")
# 配置前移除上游内嵌 JSON 单头，保证自身编译与导出的公共目标都使用 manifest 依赖。
file(REMOVE "${SOURCE_PATH}/include/nlohmann/json.hpp")
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DWITH_MKL=OFF -DWITH_DNNL=ON -DWITH_CUDA=OFF -DWITH_CUDNN=OFF
        -DWITH_OPENBLAS=OFF -DWITH_RUY=OFF -DOPENMP_RUNTIME=COMP
        -DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=ON
        -DENABLE_CPU_DISPATCH=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/ctranslate2)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE"
    "${CPU_FEATURES_SOURCE}/LICENSE" "${SPDLOG_SOURCE}/LICENSE")
