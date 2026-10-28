"""Supported release editions; retained historical tags are not new editions."""
LAST_MINIMAL_TAG = 'v0.5.1-minimal'


def require_supported_edition(tag):
    if tag.endswith('-minimal') and tag != LAST_MINIMAL_TAG:
        raise ValueError('Minimal maintenance ends at ' + LAST_MINIMAL_TAG + '; publish the bundled-Python C++ release instead')
