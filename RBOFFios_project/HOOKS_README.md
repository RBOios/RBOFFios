# 네이티브 후크 (hooks.c)

RBO_Ex3.exe(KR)의 병목 루프 3곳을 인터프리터 대신 네이티브 C로 실행합니다.

| 주소 | 역할 | 효과 |
|---|---|---|
| 0x414d10 | 이미지 → 서피스 변환 | 스테이지/캐릭터 로딩 |
| 0x415060 | 사각형 영역 복사 | 스테이지/캐릭터 로딩 |
| 0x467169 | 매 프레임 빈 슬롯(-1) 스캔 | 타이틀/메뉴 CPU 사용 |

- 설치 시 원본 바이트를 확인하며, 다른 빌드의 exe는 그대로 둡니다.
- 지원하지 않는 모드/포맷은 원래 코드로 되돌아갑니다.
- 환경변수: `RBO_NOHOOK=1` 끄기, `RBO_HOOKMASK=1|2|4` 일부만 켜기(1=conv 2=rect 4=scan), `RBO_HOOKCHECK=1` 호출마다 원본 코드와 대조(느림, 디버그용)

## 테스트 (리눅스)
    make test_hooks && ./test_hooks /path/to/RBO_Ex3.exe 1500
원본 인터프리터 결과와 후크 결과를 무작위 입력으로 비교합니다.

## 변경 파일
hooks.c(신규), test_hooks.c(신규), hle.c(nargs==-2 raw 후크), main.c(설치 호출), Makefile, ios/project.yml

# 래스터라이저 최적화 (d3d.c)
픽셀 경로(`shade_px2`)를 정확도 보존 방식으로 최적화했습니다.
- 렌더 상태를 로컬 복사해 레지스터에 유지, 픽셀 패킹을 분기 없는 시프트로 계산
- 흰색 디퓨즈 + 텍스처 modulate는 텍스처 값을 그대로 사용 (mul8(255,x)==x 전수 확인)
- SRCALPHA/INVSRCALPHA에서 알파가 255면 목적지 읽기/블렌딩 생략
- 원본 구현은 `shade_px_ref`로 보존(`-DRBO_RASTCHECK`일 때만 컴파일)

테스트: `make test_raster && ./test_raster 30000` — 원본과 새 구현이 같은 바이트를 쓰는지 무작위 픽셀로 비교

# 2차 래스터 최적화 (최신 소스 기준)
- 단색(flat) + 텍스처 없음 삼각형: 알파 테스트/SRCALPHA 상수를 픽셀 루프 밖으로 이동 (`tri_rows`의 fastmode)
- 이중선형 샘플링: 내부 영역은 wrapc 생략, 두 채널씩 묶어 계산(SWAR) — 결과 동일
- 원본은 `tri_rows_ref`/`shade_px_ref`로 보존(-DRBO_RASTCHECK). `make test_raster`는 픽셀 단위 + 삼각형 단위로 비교

# 네이티브 해상도 표시 (dx.c / MetalPresenter.swift)
게임이 640x480을 그린 뒤 가짜 창(1280x1024)으로 확대하는 Blt를 1:1 복사로 바꾸고, 확대는 GPU(MetalPresenter)가 4:3 비율로 합니다.
- CPU의 확대 + 1280x1024 변환(프레임당 약 1.2ms, PC 측정)이 640x480 복사 + 변환(약 0.2ms)으로 줄어듭니다.
- 런처의 "화면 크기" 선택을 되살렸습니다(640x480 / 가상 PSP / 800x600 / 1024x768). 고른 값을 게임 설정(SystemConfig.v4)에 기록하고, 화면 표시 비율은 4:3(가상 PSP만 480:272)입니다. 게임이 그리는 그림은 항상 640x480이고, 항목마다 달라지는 건 내부 버퍼 크기입니다.

# 파일 경로 캐시 (hle.c)
`resolve_path`(대소문자 무시 경로 찾기)가 파일을 열 때마다 폴더 목록을 훑던 것을, 존재하는 경로에 한해 캐시합니다(없는 파일은 캐시하지 않음, 적중 시 access()로 재확인).
테스트: `make test_resolve && ./test_resolve <게임 폴더>` — 기존 방식과 결과가 같은지 비교합니다.

# 640x480 항목의 게임 버퍼 (dx.c)
원래 이 항목은 1280x1024 버퍼를 만들고 왼쪽 위 640x480에만 그립니다. 버퍼 자체를 640x480으로 만들도록 바꿨습니다(1280x1024 크기의 오프스크린 서피스만 해당, 다른 항목은 그대로). 환경변수 `RBO_NOSHRINK=1`로 끌 수 있습니다. 리눅스 에뮬레이터의 타이틀·데모 전투 화면에서 기존과 같게 나오는 것까지만 확인했습니다.

# 사운드 버퍼 슬롯 반환 (dx.c) — rbo_crash.txt 대응
`IDirectSoundBuffer::Release`가 아무것도 하지 않아서, 스테이지/전투를 거칠 때마다 사운드 버퍼 슬롯(512개)이 쌓이기만 했습니다. 슬롯이 가득 차면 `SB[-1]`을 건드려 `IDirectSoundBuffer::SetVolume`에서 크래시했습니다(크래시 주소 = `&SB[0] + 200 * 0xFFFFFFFF`).
- 사운드 버퍼에 참조 카운트를 추가: `CreateSoundBuffer`/`DuplicateSoundBuffer`는 1, `AddRef`/`QueryInterface`(같은 객체)는 +1, `Release`는 -1이고 0이 되면 슬롯과 게임 메모리를 반환합니다(복제본이 같은 데이터를 쓰는 동안은 메모리를 유지).
- 이미 해제된 버퍼에 대한 `Release`는 무시, 해제된 객체로 들어오는 호출은 `sb_dummy`가 받아서 호스트 크래시가 나지 않습니다.
- 슬롯이 가득 차면 `E_OUTOFMEMORY`를 돌려주고 로그(`all 512 slots in use`)를 남깁니다.
