// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's update check (launcher_gui/update_logic.h): version order, the daily limit, the
// .sha256 file and the hash check, the GitHub release JSON, and the paths an update may write.
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../launcher_gui/update_logic.h"

using namespace GolfUpdate;

static int compare(const char* a, const char* b) {
    Version x, y;
    assert(ParseVersion(a, x) && ParseVersion(b, y));
    return CompareVersions(x, y);
}

int main() {
    // Versions: "v" optional, three numbers, an optional pre-release part, build metadata ignored.
    Version v;
    assert(ParseVersion("v0.1.0", v) && v.major == 0 && v.minor == 1 && v.patch == 0 && v.pre.empty());
    assert(ParseVersion("12.3.45", v) && v.major == 12 && v.minor == 3 && v.patch == 45);
    assert(ParseVersion("v1.0.0-rc.1+build5", v) && v.pre == "rc.1");
    for (const char* bad : {"", "v", "1", "1.2", "1.2.3.4", "v1..2", "1.2.x", "development", "a1b2c3d", "v0.1.0-",
                            "v0.1.0-dirty space", "v0.1.0-12-gabcdef0 ", "v0.1.0-5-gabc1234", "v0.1.0-5-gabc1234-dirty", "v0.1.0-dirty", "1.2.3junk"})
        assert(!ParseVersion(bad, v));

    assert(compare("v0.1.0", "v0.1.0") == 0);
    assert(compare("v0.1.1", "v0.1.0") == 1);
    assert(compare("v0.2.0", "v0.1.9") == 1);
    assert(compare("v1.0.0", "v0.99.99") == 1);
    assert(compare("v0.10.0", "v0.9.0") == 1); // numbers, not text
    assert(compare("v0.1.0", "0.1.0") == 0);
    assert(compare("v0.2.0-rc1", "v0.2.0") == -1); // a pre-release comes before its release
    assert(compare("v0.2.0-rc1", "v0.1.0") == 1);
    assert(compare("v0.2.0-rc.2", "v0.2.0-rc.10") == -1);
    assert(compare("v0.2.0-1", "v0.2.0-alpha") == -1);
    assert(compare("v0.2.0-alpha", "v0.2.0-alpha.1") == -1);
    assert(compare("v0.2.0-beta", "v0.2.0-alpha") == 1);

    assert(IsNewer("v0.1.1", "v0.1.0"));
    assert(!IsNewer("v0.1.0", "v0.1.0"));
    assert(!IsNewer("v0.0.9", "v0.1.0"));
    assert(!IsNewer("v9.9.9", "a1b2c3d-dirty")); // development builds never update
    assert(!IsNewer("nightly", "v0.1.0"));

    // Once a day at most; off means never; a clock that went back checks again.
    const long long day = 24 * 60 * 60, now = 1'800'000'000;
    assert(CheckDue(true, 0, now));
    assert(!CheckDue(true, now - day + 1, now));
    assert(CheckDue(true, now - day, now));
    assert(CheckDue(true, now + 60, now));
    assert(!CheckDue(false, 0, now));

    // SHA-256 (FIPS 180-2 vectors), across block boundaries and in pieces.
    assert(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 s;
        const std::string a(1000, 'a');
        for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size());
        assert(s.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    // The .sha256 file beside a zip: sha256sum's format, with or without the name.
    const std::string abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    assert(ParseSha256File(abc + "  golf-v0.1.0-windows-x64.zip\n", "golf-v0.1.0-windows-x64.zip") == abc);
    assert(ParseSha256File(abc + " *golf-v0.1.0-windows-x64.zip\r\n", "golf-v0.1.0-windows-x64.zip") == abc);
    assert(ParseSha256File(abc + "\n") == abc);
    assert(ParseSha256File("\xef\xbb\xbf" "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  x.zip", "x.zip") == abc);
    assert(ParseSha256File(abc + "  other.zip\n", "golf.zip").empty()); // names another file
    assert(ParseSha256File(abc.substr(0, 63) + "\n").empty());
    assert(ParseSha256File(abc + "0  x.zip").empty()); // 65 digits
    assert(ParseSha256File("g" + abc.substr(1)).empty());
    assert(ParseSha256File("").empty());

    // The hash check: equal digests pass (any case), anything else refuses, an empty digest too.
    assert(HashMatches(abc, Sha256Hex("abc")));
    assert(HashMatches("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", Sha256Hex("abc")));
    assert(!HashMatches(abc, Sha256Hex("abd")));
    assert(!HashMatches("", Sha256Hex("abc")));
    assert(!HashMatches(abc, ""));
    assert(!HashMatches(ParseSha256File("not a digest"), Sha256Hex("abc")));

    // The latest-release JSON: the fields used, escapes, nulls, other fields skipped.
    const char* json = R"({
      "url": "https://api.github.com/repos/o/r/releases/1", "id": 123, "draft": false, "prerelease": false,
      "tag_name": "v0.2.0", "name": "Everybody's Golf PC v0.2.0",
      "html_url": "https://github.com/o/r/releases/tag/v0.2.0",
      "author": {"login": "someone", "id": 5, "site_admin": false},
      "body": "Fixes:\r\n- the \"rough\" lie\n- caf\u00e9 \ud83c\udfcc text\\path",
      "assets": [
        {"name": "everybodys-golf-pc-v0.2.0-symbols.zip", "size": 10, "browser_download_url": "https://e/s.zip"},
        {"name": "everybodys-golf-pc-v0.2.0-windows-x64.zip.sha256", "browser_download_url": "https://e/z.zip.sha256",
         "uploader": null, "label": null},
        {"name": "everybodys-golf-pc-v0.2.0-windows-x64.zip", "size": 1.5e8, "browser_download_url": "https://e/z.zip"}
      ],
      "reactions": {"+1": 2, "list": [1, 2.5, -3, true, null, "x", {"a": []}]}
    })";
    Release r;
    assert(ParseRelease(json, r));
    assert(r.tag == "v0.2.0" && r.name == "Everybody's Golf PC v0.2.0");
    assert(r.page == "https://github.com/o/r/releases/tag/v0.2.0");
    assert(r.notes == "Fixes:\r\n- the \"rough\" lie\n- caf\xc3\xa9 \xf0\x9f\x8f\x8c text\\path");
    assert(r.assets.size() == 3);
    Asset zip, sha;
    assert(PickWindowsAssets(r, zip, sha));
    assert(zip.url == "https://e/z.zip" && sha.url == "https://e/z.zip.sha256");
    r.assets.erase(r.assets.begin() + 1); // no .sha256: no update
    assert(!PickWindowsAssets(r, zip, sha));
    assert(!ParseRelease("", r));
    assert(!ParseRelease("{\"message\": \"Not Found\"}", r)); // no tag
    assert(!ParseRelease("{\"tag_name\": \"v1.0.0\"", r));     // cut short
    assert(!ParseRelease("{\"tag_name\": \"v1.0.0\"} x", r));
    assert(ParseRelease("{\"tag_name\": \"v1.0.0\", \"body\": null}", r) && r.notes.empty());

    // Drafts and pre-releases are never offered; a 403/rate-limit body is not a release; a tag that is
    // not a version never counts as newer; a development build is never offered its own release.
    assert(ParseRelease("{\"tag_name\": \"v1.0.0\", \"draft\": true}", r) && r.draft && !Offerable(r));
    assert(ParseRelease("{\"tag_name\": \"v1.0.0\", \"prerelease\": true, \"draft\": false}", r) && r.prerelease && !Offerable(r));
    assert(ParseRelease("{\"tag_name\": \"v1.0.0\", \"prerelease\": false, \"draft\": null}", r) && Offerable(r));
    assert(!ParseRelease("{\"message\": \"API rate limit exceeded\", \"documentation_url\": \"https://x\"}", r));
    assert(!IsNewer("nightly", "v0.1.0") && !IsNewer("latest", "v0.1.0") && !IsNewer("v2", "v0.1.0"));
    assert(!IsNewer("v0.1.0", "v0.1.0-5-gabc1234") && !IsNewer("v0.1.1", "development"));
    assert(IsNewer("v0.1.1", "v0.1.0"));
    {
        // Cleanup after any outcome: staging and .part files go, the journal and the player's files stay.
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "golf-update-test";
        fs::remove_all(base);
        const fs::path up = base / "data" / "update";
        fs::create_directories(up / "staging" / "libs");
        fs::create_directories(base / "data" / "save");
        std::ofstream(base / "data" / "save" / "settings.ini") << "x";
        std::ofstream(up / "staging" / "libs" / "a.dll") << "x";
        std::ofstream(up / "z.zip.part") << "x";
        std::ofstream(up / "journal.txt") << "R a";
        CleanUpdateDir(up);
        assert(!fs::exists(up / "staging") && !fs::exists(up / "z.zip.part"));
        assert(fs::exists(up / "journal.txt") && fs::exists(base / "data" / "save" / "settings.ini"));
        fs::remove(up / "journal.txt");
        fs::create_directories(up / "staging"); // the empty leftover of a refused update
        CleanUpdateDir(up);
        assert(!fs::exists(up) && fs::exists(base / "data" / "save" / "settings.ini"));
        CleanUpdateDir(up); // missing folder: nothing to do
        fs::remove_all(base);
    }

    // Zip entries: inside the release's top folder only.
    assert(InstallPath("everybodys-golf-pc-v0.2.0/EverybodysGolf.exe") == "EverybodysGolf.exe");
    assert(InstallPath("top\\libs\\a.dll") == "libs/a.dll");
    for (const char* bad : {"EverybodysGolf.exe", "top/", "top/libs/", "/top/x", "C:/x/y", "top/../x", "top/a/../../x",
                            "top/./x", "top//x", "top/a:b"})
        assert(InstallPath(bad).empty());

    // The player's files are never replaced.
    for (const char* own : {"data/save/settings.ini", "DATA/game/x", "save/savedata/a", "game/image-1/boot.bin", "log/x.log",
                            "settings.ini", "Paths.ini", "shaders/x", "update/x"})
        assert(PlayerOwned(own));
    for (const char* ship : {"EverybodysGolf.exe", "everybodys-golf.exe", "libs/libs.manifest", "tools/python/python.exe",
                             "settings.example.ini", "README.txt", "release.txt", "database.txt", "gamepad.dll"})
        assert(!PlayerOwned(ship));

    std::puts("launcher update: ok");
    return 0;
}
