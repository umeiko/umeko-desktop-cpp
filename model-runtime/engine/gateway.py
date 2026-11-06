"""URL contract shared with the native gateway; no GUI imports."""
from urllib.parse import urlsplit


def validate_base_url(value):
    url = urlsplit(value.strip())
    if (url.scheme not in ('http', 'https') or not url.hostname or
            url.username or url.password or url.query or url.fragment):
        raise ValueError('Invalid provider base URL')
    return value.strip().rstrip('/')
