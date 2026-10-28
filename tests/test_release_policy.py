import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from release_policy import require_supported_edition


class ReleasePolicyTests(unittest.TestCase):
    def test_current_final_minimal_can_still_be_repaired(self):
        require_supported_edition('v0.5.1-minimal')

    def test_future_minimal_is_not_published(self):
        for tag in ('v0.5.2-minimal', 'v0.6.0-minimal', 'v1.0.0-minimal'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                require_supported_edition(tag)

    def test_standard_releases_remain_supported(self):
        for tag in ('v0.5.1', 'v0.5.2', 'v0.6.0', 'v1.0.0'):
            require_supported_edition(tag)
