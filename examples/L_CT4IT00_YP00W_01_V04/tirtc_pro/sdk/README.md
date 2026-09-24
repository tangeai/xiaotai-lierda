<!-- SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai> -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# TiRTC SDK placement

The headers and prebuilt archive below are proprietary TiRTC SDK materials.
They are not covered by the MIT or Apache-2.0 licences applied to the product
application source:

- `include/tirtc/*`
- `lib/libTiRTC.a`

They may be used or redistributed only under the applicable TiRTC SDK agreement
or separate written authorization from the TiRTC owner. This release includes
the matching headers and archive required to build the application; public
distribution of this supplied SDK has been confirmed for this release. This
does not relicense the SDK or expand the rights granted by its own agreement.

The upstream release's notices are retained verbatim in
[UPSTREAM_THIRD_PARTY_NOTICES.md](UPSTREAM_THIRD_PARTY_NOTICES.md).
Its component paths describe the upstream tree, not this reduced integration.
File hashes and local adaptation provenance are recorded in
[SOURCE_PROVENANCE.md](../runtime/SOURCE_PROVENANCE.md).
A hash identifies bytes; it is not a grant of rights or proof of version
provenance. Integration details are in [runtime/README.md](../runtime/README.md).

## 阅读上游原始通知

`UPSTREAM_THIRD_PARTY_NOTICES.md` 按上游原文保留，其中相对链接按原工程目录解释，不能从本文件所在目录直接解析；其 OLED/Opus 使用描述属于上游产品，不表示当前触屏应用启用了 Opus。当前音频使用 G.711 A-law。

当前可用的许可入口：

- [仓库 LICENSE](../../../../LICENSE)
- [应用适配 MIT 副本](../runtime/LICENSES/MIT.txt)
- [应用适配 Apache-2.0 副本](../runtime/LICENSES/Apache-2.0.txt)
- [cJSON 许可头](../../../../components/thirdparty/CJSON/cJSON.h)
- [Opus 上游许可（公共 SDK 保留组件，当前应用未启用）](../../../../components/thirdparty/opus/opus-1.4/COPYING)
- [保留的 mbedTLS 许可](../board_compat/licenses/mbedtls-LICENSE)

以上仅修正文档阅读路径，不修改任何原始许可文本。
