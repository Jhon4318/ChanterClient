# Third-party notices

ChanterClient uses or is derived from the following open-source projects. Dependency versions are pinned in `CMakeLists.txt`.

- **MCBE_DX12_ModBase** by Binary01 — MIT License. The initial internal-client architecture and Minecraft SDK integration are adapted from this project. The original copyright notice is retained in `LICENSE`.
  - https://github.com/b01dev/MCBE_DX12_ModBase
- **Dear ImGui** — MIT License. The license text is included at `external/imgui/LICENSE.txt`.
  - https://github.com/ocornut/imgui
- **EnTT 3.14.0** — MIT License.
  - https://github.com/skypjack/entt
- **libhat 0.11.1** — MIT License.
  - https://github.com/BasedInc/libhat
- **MinHook 1.3.3** — BSD 2-Clause License.
  - https://github.com/TsudaKageyu/minhook
- **nlohmann/json 3.11.2** — MIT License.
  - https://github.com/nlohmann/json
- **expected-lite 0.9.0** — Boost Software License 1.0.
  - https://github.com/nonstd-lite/expected-lite
- **Endstone Spyglass** — MIT License. Used as a public reference for the Bedrock packet read hook layout and version-specific `MinecraftPackets::createPacket` signatures; no Spyglass implementation files are copied into ChanterClient.
  - https://github.com/EndstoneMC/spyglass
- **DirectX helper header (d3dx12.h)** — Microsoft DirectX Graphics Samples; MIT License.
  - https://github.com/microsoft/DirectX-Graphics-Samples

The dependencies listed above are fetched at configure time by CMake, except Dear ImGui and the DirectX helper header, which are kept in this repository. Refer to each upstream project for the full license text and notices.
