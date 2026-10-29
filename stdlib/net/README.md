# net

Blocking and non-blocking TCP and UDP sockets, DNS resolution, and TLS client
connections. Sockets are `i64` handles owned by the process; close them
explicitly with `net.close`.

```a
import net

listener = net.tcp_listen("127.0.0.1", 0)     # 0 picks an ephemeral port
port = net.local_port(listener)
client = net.tcp_connect("127.0.0.1", port)
server = net.accept(listener)
net.send(server, "hello")
print(net.recv(client, 64))
net.close(client)
net.close(server)
net.close(listener)
```

| Member | Signature | Result |
| ------ | --------- | ------ |
| `resolve` | `(host)` | `[String]` numeric addresses, in the order the resolver returns them |
| `tcp_listen` | `(host, port)` | listening handle; `port` 0 asks the OS for one |
| `tcp_connect` | `(host, port)` | connected handle |
| `accept` | `(listener)` | next incoming connection; blocks unless a timeout is set |
| `send` | `(handle, data)` | bytes written |
| `recv` | `(handle, maxBytes)` | up to `maxBytes` bytes; `""` at EOF or when nothing is waiting |
| `close` | `(handle)` | releases the socket; a second close is a runtime error |
| `local_port` | `(handle)` | the bound port |
| `set_timeout` | `(handle, millis)` | send/receive timeout, `0` clears it |
| `set_nonblocking` | `(handle, bool)` | non-blocking mode |
| `poll` | `([i64], timeoutMillis)` | handles that are ready to read |
| `udp_socket` | `()` | unbound datagram handle |
| `udp_bind` | `(handle, host, port)` | binds it (`port` 0 = ephemeral) |
| `udp_send` | `(handle, host, port, data)` | bytes sent |
| `udp_recv` | `(handle, maxBytes)` | `(payload, peerHost, peerPort)`; `("", "", -1)` when nothing is waiting |
| `tls_connect` | `(host, port)` | TLS client handle |
| `tls_available` | `()` | whether the toolchain was built with OpenSSL |

Reads never return partial-parse errors: a timeout or an idle non-blocking
socket yields an empty string, so poll or set a timeout before blocking reads.
Every socket operation that fails at the OS level is a runtime error carrying
the system message (`Connection refused`, `Address in use`, and so on).

TLS support is compiled in when OpenSSL is available at build time; otherwise
`tls_available()` is `false` and `tls_connect` reports that TLS support is not
built in. TLS connections use the platform trust store, verify the certificate
against the connecting host name, and are otherwise ordinary sockets for
`send`/`recv`/`poll`/`close`. There is no TLS listener — terminate TLS with a
proxy or a dedicated server if you need one.
