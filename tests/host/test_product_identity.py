"""HardlyAFK release identity checks; run with python -m unittest discover -s tests/host -p 'test_product_identity.py'."""
import pathlib
import plistlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ProductIdentity(unittest.TestCase):
    def test_bundle_id_and_display_name(self):
        config = (ROOT / "app/iSH.xcconfig").read_text()
        self.assertRegex(config, r"(?m)^ROOT_BUNDLE_IDENTIFIER = ing\.hardlysleep\.HardlyAFK$")
        info = plistlib.loads((ROOT / "app/Info.plist").read_bytes())
        self.assertEqual(info["CFBundleDisplayName"], "HardlyAFK")
        self.assertEqual(info["NSUserActivityTypes"], ["ing.hardlysleep.HardlyAFK.scene"])

    def test_no_live_upstream_publishing_credentials(self):
        active = [ROOT / "app/iSH.xcconfig", ROOT / "app/App.xcconfig", ROOT / "app/Info.plist", ROOT / "iSH.xcodeproj/project.pbxproj"]
        active += list((ROOT / "fastlane").glob("*")) if (ROOT / "fastlane").exists() else []
        active += list((ROOT / ".github/workflows").glob("*.yml"))
        forbidden = re.compile(r"app\.ish\.iSH|tblodt@icloud\.com|CK5SXRTBR7|K8Q74NT2BG|ish-app/certificates|ish-app/ish")
        for path in active:
            with self.subTest(path=path):
                self.assertIsNone(forbidden.search(path.read_text()), f"upstream publishing identity in {path}")

    def test_icon_set_covers_all_declared_sizes(self):
        import json
        directory = ROOT / "app/Assets.xcassets/AppIcon.appiconset"
        catalog = json.loads((directory / "Contents.json").read_text())
        for icon in catalog["images"]:
            with self.subTest(icon=icon):
                data = (directory / icon["filename"]).read_bytes()
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                self.assertGreater(len(data), 100)


if __name__ == "__main__":
    unittest.main()
