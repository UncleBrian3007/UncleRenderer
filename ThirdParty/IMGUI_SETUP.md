# ImGui 의존성 준비 방법

프로젝트에 포함된 FPS 오버레이는 Dear ImGui 헤더와 소스가 있을 때만 컴파일됩니다. 현재 저장소에는 라이브러리가 포함되어 있지 않으므로 아래 단계로 로컬에 가져와야 합니다.

1. 공식 저장소를 클론하거나 ZIP 으로 내려받습니다.
   - 저장소 주소: https://github.com/ocornut/imgui
2. 압축을 풀어 `ThirdParty/imgui` 폴더에 배치합니다. (예: `ThirdParty/imgui/imgui.h` 형태로 존재해야 함)
3. Visual Studio 솔루션을 다시 로드하면 자동으로 `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp` 와 백엔드 `backends/imgui_impl_dx12.cpp`, `backends/imgui_impl_win32.cpp` 를 빌드에 포함합니다.

ImGui 파일이 없으면 빌드는 계속 진행되지만 UI 렌더링이 비활성화됩니다. 실제 FPS 오버레이를 확인하려면 위 위치에 라이브러리를 내려받아 주세요.
