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
