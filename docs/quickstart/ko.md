> 이 페이지는 영어 빠른 시작의 번역으로, AI 모델로 만들고 두 번째 AI 모델이 검토했습니다. 아직 원어민 검토는 받지 않았습니다.

# 빠른 시작: Midi Fadester와 Visual Sidechain

## Midi Fadester

Midi Fadester는 MIDI 컨트롤러의 페이더와 노브를 OBS Studio의 라이브 컨트롤로 바꿉니다. 진짜 크로스페이더로 장면 사이를 페이드하고, 두 레이어를 믹스하고, 카메라를 페이드하고, 볼륨을 조절하고, 어떤 필터 설정이든 움직일 수 있습니다. Akai APC40 mkII용으로 만들었고, 다른 컨트롤러는 MIDI Learn으로 사용할 수 있습니다.

### 빠른 시작

<img src="../../docs/quick-start.svg" alt="Midi Fadester 3단계: 1, 최신 릴리스의 설치 프로그램을 실행하고 OBS 시작; 2, 컨트롤러 연결 (APC40 mkII는 자동 설정); 3, 스튜디오 모드를 켜고 크로스페이더를 밀기: 프로그램이 미리보기로 페이드." width="900">

1. **설치하세요.** [최신 릴리스](https://github.com/jbrick2070/midi-fadester/releases/latest)에서 `midi-fadester-<version>-windows-x64-Installer.exe`(Windows) 또는 `midi-fadester-<version>-macos-universal.pkg`(Mac)를 실행한 다음 OBS Studio 33 이상을 시작하세요. 다른 파일과 주의 사항: [Install](../../README.md#install)(영어).
2. **컨트롤러를 연결하세요.** APC40 mkII는 자동으로 설정되고 **Fadester Desk** 독이 열립니다. 다른 컨트롤러: **도구 → Midi Fadester**(OBS 위쪽 메뉴 막대)를 열고 **스트립** 탭에서 **컨트롤러 변경**을 클릭하세요.
3. **크로스페이더를 미세요.** OBS의 **스튜디오 모드** 버튼(오른쪽 아래)을 클릭한 다음 크로스페이더를 반대쪽 끝까지 미세요. 프로그램이 미리보기에 있던 화면으로 페이드합니다.

끝입니다. 페이더에 다른 역할을 주려면 **도구 → Midi Fadester**에서 그 스트립 아래의 **설정**을 클릭하세요.

**[전체 안내 (영어)](../../README.md)**

## Visual Sidechain

Visual Sidechain은 Windows와 macOS의 OBS Studio 33에서 영상이 음악에 맞춰 움직이게 합니다. 0.9.0부터 Midi Fadester에 포함되어 있어 Midi Fadester를 설치하면 바로 쓸 수 있습니다.

### 빠른 시작

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain 3단계: 1, Midi Fadester 설치 (Visual Sidechain 포함); 2, 소스의 필터에서 +를 눌러 Visual Sidechain: 셰이더 필터 추가; 3, 음악을 재생하면 화면이 소리에 맞춰 움직임." width="900">

1. **Midi Fadester를 설치**하고([Install](../../README.md#install), 영어) OBS Studio 33을 시작하세요.
2. **필터를 추가하세요.** 소스나 장면을 마우스 오른쪽 버튼으로 클릭하고 **필터**를 선택한 뒤, 효과 필터 아래의 **`+`**를 눌러 **`Visual Sidechain: 셰이더 필터`**를 추가하세요.
3. **음악을 재생하세요.** 화면이 **트랙 1 (메인 믹스)**의 소리에 맞춰 움직입니다.

끝입니다. 필터 창의 **효과**에서 다른 모습을 고를 수 있습니다.

> **방송 전에 긴급 정지 키를 지정하세요:** **설정 → 단축키 → `Visual Sidechain: 긴급 정지 (모든 효과 끄기)`**. 한 번 누르면 모든 장면의 Visual Sidechain 효과가 꺼집니다.

**[전체 안내 (영어)](../VISUAL-SIDECHAIN.md)**
