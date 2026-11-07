import unittest
from scripts.release_retention import obsolete_patches, prune_releases


def release(tag, identifier=1, **changes):
    return dict(tag_name=tag, id=identifier, draft=False, assets=[{}]*15, target_commitish="fixture-commit", **changes)


class ReleaseRetentionTests(unittest.TestCase):
    def test_windows_plan_requires_the_exact_verified_asset_set(self):
        names = ['windows.zip', 'windows.zip.sha256', 'windows.json',
                 'provider-sdk.zip', 'develop-provider.zip', 'SHA256SUMS.txt']
        current = release('v0.7.1'); current['assets'] = [{'name': name} for name in names]
        rows = [current, release('v0.7.0', 2), release('v0.6.5', 3)]; calls = []
        with self.assertRaises(AssertionError):
            prune_releases('umeiko/umeko-desktop-cpp', 'v0.7.1', lambda _: rows, lambda *args: calls.append(args))
        self.assertEqual(prune_releases('umeiko/umeko-desktop-cpp', 'v0.7.1', lambda _: rows,
                                       lambda *args: calls.append(args), verified_assets=names), [])
        for assets in (current['assets'][:-1], current['assets'][:-1]+[{'name': 'foreign.zip'}],
                       current['assets'][:-1]+[current['assets'][0]]):
            original = current['assets']; current['assets'] = assets
            with self.assertRaises(AssertionError):
                prune_releases('umeiko/umeko-desktop-cpp', 'v0.7.1', lambda _: rows,
                               lambda *args: calls.append(args), verified_assets=names)
            current['assets'] = original
        self.assertEqual(calls, [])

    def test_standard_release_after_minimal_retirement_needs_no_pair(self):
        rows = [release('v0.5.0', 1), release('v0.5.1', 2), release('v0.5.1-minimal', 3), release('v0.5.2', 4)]
        calls = []
        self.assertEqual(prune_releases('umeiko/umeko-desktop-cpp', 'v0.5.2', lambda _: rows, lambda *args: calls.append(args)), ['v0.5.1', 'v0.5.1-minimal'])
        self.assertEqual(len(calls), 2)

    def test_new_dmg_releases_have_nineteen_assets(self):
        old = release("v0.3.1", 2)
        current = release("v0.3.8", 3); current['assets'] = [{}] * 19
        minimal = release("v0.3.8-minimal", 4); minimal['assets'] = [{}] * 19
        rows = [old, current, minimal]; calls = []
        self.assertEqual(prune_releases("umeiko/umeko-desktop-cpp", "v0.3.8", lambda _: rows, lambda *args: calls.append(args)), ["v0.3.1"])
        self.assertEqual(len(calls), 1)

    def test_keeps_milestones_other_series_future_versions_and_drafts(self):
        tags = ["v0.2.9", "v0.3.0", "v0.3.1", "v0.3.3-minimal", "v0.3.4", "v0.3.4-minimal", "v0.3.5", "voices-v1"]
        rows = [release(tag, i) for i, tag in enumerate(tags)]
        draft = release("v0.3.2"); draft["draft"] = True; rows.append(draft)
        self.assertEqual([r["tag_name"] for r in obsolete_patches(rows, "v0.3.4")], ["v0.3.1", "v0.3.3-minimal"])
        self.assertEqual(obsolete_patches(rows, "v0.3.0"), [])

    def test_waits_for_both_native_editions_and_deletes_only_release_endpoints(self):
        rows = [release("v0.3.0"), release("v0.3.2", 2), release("v0.3.4-minimal", 4)]
        calls = []
        self.assertEqual(prune_releases("umeiko/umeko-desktop-cpp", "v0.3.4-minimal", lambda _: rows, lambda *args: calls.append(args)), [])
        self.assertEqual(calls, [])
        rows.append(release("v0.3.4", 5))
        self.assertEqual(prune_releases("umeiko/umeko-desktop-cpp", "v0.3.4", lambda _: rows, lambda *args: calls.append(args)), ["v0.3.2"])
        self.assertEqual(calls, [("api", "--method", "DELETE", "repos/umeiko/umeko-desktop-cpp/releases/2")])
