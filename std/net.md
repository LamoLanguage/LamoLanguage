# std.net

Simple blocking HTTP client. The first version supports plain HTTP
only; HTTPS support is planned for a future release (it requires
linking against a TLS library, which the language doesn't do yet).

Both functions block until the response is fully received and return
the response body as a string. On network or parsing errors, they
return an empty string.

## Function Reference

- `get(url)` — issue an HTTP GET and return the response body.
- `post(url, body)` — issue an HTTP POST with the given body and return the response body. Content-Type is `text/plain`.
- `httpGet(url)` — explicit alias for `get(url)`.
- `httpPost(url, body)` — explicit alias for `post(url, body)`.

## Examples

```lamo
import std.net as net

let body = net.get("http://example.com/")
print(body)

let r = net.post("http://httpbin.org/post", "hello=world")
print(r)
```

## Notes

- **No HTTPS.** Only `http://` URLs are supported in this release.
- The request sets `Connection: close`, so the server closes the
  socket after the response — there is no streaming API.
- A failed request (DNS failure, refused connection, malformed
  response) returns `""`. Distinguish a real empty response from an
  error by checking the return value's length before relying on it.
