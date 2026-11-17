import SwiftUI

/// App UI language: Korean or English. The first launch follows the iPhone/iPad language (Korean -> Korean, anything else -> English);
/// the launcher has a 한국어 | English switch that overrides it and is remembered.
/// (The game's own text comes from the original Korean game files and is not affected.)
enum AppLang: String, CaseIterable, Identifiable {
    case ko, en
    var id: String { rawValue }
    var label: String { self == .ko ? "한국어" : "English" }
    static let key = "rboLang"
    static var systemDefault: AppLang { (Locale.preferredLanguages.first ?? "en").hasPrefix("ko") ? .ko : .en }
    static var current: AppLang { AppLang(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? systemDefault }
}

/// tr("한국어 문구") -> the text in the current language. The Korean text is the key; English lives in L10n.en.
/// For texts with a value, use tr("폴더: %@") with String(format:) or trf().
func tr(_ ko: String) -> String { AppLang.current == .ko ? ko : (L10n.en[ko] ?? ko) }
func trf(_ ko: String, _ args: CVarArg...) -> String { String(format: tr(ko), arguments: args) }

enum L10n {
    static let en: [String: String] = [
        // launcher
        "확장팩 3": "Expansion 3",
        "%@ 시작하기": "Start %@",
        "화면 크기": "Screen size",
        "가상 PSP": "Virtual PSP",
        "게임 폴더 선택…": "Select game folder…",
        "게임 폴더가 선택되지 않았어요 (RBO_Ex3.exe와 DATA 폴더가 있는 곳)": "No game folder selected (the one that contains RBO_Ex3.exe and the DATA folder)",
        "폴더: %@": "Folder: %@",
        "폴더 접근 권한을 얻지 못했어요.": "Couldn't get access to the folder.",
        "먼저 게임 폴더를 선택해 주세요.": "Please select the game folder first.",
        "%@ 파일을 찾지 못했어요.": "Couldn't find %@.",
        // game ended screen
        "게임이 종료되었어요": "The game has ended",
        "다시 시작하려면 앱을 다시 실행해 주세요 (엔진 상태가 전역이라 한 번만 시작할 수 있어요).": "To play again, please relaunch the app (the engine can only be started once per launch).",
        "앱 종료": "Quit app",
        // quick save / load
        "저장한 게임을 불러올까요?": "Load the saved game?",
        "취소": "Cancel",
        "불러오기": "Load",
        "저장 중…": "Saving…",
        "불러오는 중…": "Loading…",
        "잠시 후 다시 시도해 주세요": "Please try again in a moment",
        "저장된 게임이 없어요": "No saved game",
        "%@에 저장한 상태예요.\n지금 진행 중인 내용은 사라져요.": "Saved on %@.\nProgress made since then will be lost.",
        "저장했어요": "Saved",
        "불러왔어요": "Loaded",
        "지금은 저장할 수 없어요": "Can't save right now",
        "영상 재생 중에는 저장할 수 없어요": "Can't save while a video is playing",
        "파일을 만들 수 없어요": "Couldn't create the file",
        "저장 중 오류 (저장 공간이 부족할 수 있어요)": "Error while saving (storage may be full)",
        "저장 파일을 바꿀 수 없어요": "Couldn't replace the save file",
        "지금은 불러올 수 없어요": "Can't load right now",
        "저장 파일 형식이 맞지 않아요": "Unsupported save file format",
        "저장 파일이 손상되었어요": "The save file is damaged",
        "다른 실행 파일의 저장이에요": "This save belongs to a different executable",
        "이 버전과 호환되지 않는 저장이에요": "This save isn't compatible with this version",
        "파일을 열 수 없어요": "Couldn't open the file",
        "불러오기 실패 (게임을 다시 시작해 주세요)": "Load failed (please restart the game)",
        // cheat panel
        "치트": "Cheats",
        "스킬 P": "Skill P",
        "스탯 P": "Stat P",
        "화살": "Arrows",
        "돈": "Money",
        "채우기": "Set",
        "더하기": "Add",
        "0 ~ 9999 · 입력 후 옆의 적용 버튼을 누르면 실행돼요": "0 – 9999 · After entering, tap Apply next to it to run",
        "확인": "OK",
        "적용": "Apply",
        "고정": "Lock",
        // touch pad
        "특수": "Special",
        "방어": "Guard",
        "강 공격": "Heavy",
        "약 공격": "Light",
    ]
}
