"""Minimal AtlasDB client in pure Python, speaking the AtlasDB Wire Protocol v1.

Shows that any language can talk to atlasdb-server; see src/net/protocol.h for
the full message specification.

    python examples/python_client.py [HOST] [PORT]
"""

import socket
import struct
import sys

VERSION = 1
NULL, INTEGER, REAL, TEXT, BOOLEAN = range(5)


class AtlasError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        chunk = self.data[self.pos:self.pos + n]
        if len(chunk) != n:
            raise AtlasError("protocol_error", "truncated message")
        self.pos += n
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack(">H", self.take(2))[0]

    def u64(self):
        return struct.unpack(">Q", self.take(8))[0]

    def string(self):
        (n,) = struct.unpack(">I", self.take(4))
        return self.take(n).decode("utf-8")

    def value(self):
        tag = self.u8()
        if tag == NULL:
            return None
        if tag == INTEGER:
            return struct.unpack(">q", self.take(8))[0]
        if tag == REAL:
            return struct.unpack(">d", self.take(8))[0]
        if tag == TEXT:
            return self.string()
        if tag == BOOLEAN:
            return self.u8() != 0
        raise AtlasError("protocol_error", f"unknown value tag {tag}")


def encode_string(s):
    b = s.encode("utf-8")
    return struct.pack(">I", len(b)) + b


def encode_value(v):
    if v is None:
        return bytes([NULL])
    if isinstance(v, bool):  # before int: bool is a subclass of int
        return bytes([BOOLEAN, 1 if v else 0])
    if isinstance(v, int):
        return bytes([INTEGER]) + struct.pack(">q", v)
    if isinstance(v, float):
        return bytes([REAL]) + struct.pack(">d", v)
    if isinstance(v, str):
        return bytes([TEXT]) + encode_string(v)
    raise TypeError(f"unsupported parameter type {type(v).__name__}")


class PreparedStatement:
    def __init__(self, conn, name, param_count):
        self.conn, self.name, self.param_count = conn, name, param_count

    def execute(self, *params):
        self.conn._send(b"B", encode_execute(self.name, params))
        results = self.conn._read_until_ready()
        return results[-1] if results else None

    def close(self):
        self.conn._send(b"D", encode_string(self.name))
        self.conn._read_until_ready()


def encode_execute(name, params):
    return encode_string(name) + struct.pack(">H", len(params)) + b"".join(encode_value(v) for v in params)


class Connection:
    def __init__(self, host="127.0.0.1", port=5480, name="python-client"):
        self.sock = socket.create_connection((host, port))
        self._send(b"H", b"AWP1" + struct.pack(">H", VERSION) + encode_string(name))
        kind, body = self._recv()
        if kind == b"E":
            raise self._error(body)
        r = Reader(body)
        r.u16()
        self.server_name = r.string()
        self.in_transaction = self._recv()[1] == b"T"

    def _send(self, kind, body):
        self.sock.sendall(kind + struct.pack(">I", len(body)) + body)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise AtlasError("io_error", "server closed the connection")
            buf += chunk
        return buf

    def _recv(self):
        header = self._recv_exact(5)
        (length,) = struct.unpack(">I", header[1:])
        return header[:1], self._recv_exact(length)

    @staticmethod
    def _error(body):
        r = Reader(body)
        return AtlasError(r.string(), r.string())

    def query(self, sql, params=None):
        """Runs SQL; returns a list of results (dicts with columns/rows, or a tag).

        With `params`, the SQL must be a single statement using $1.. or ?
        placeholders; the values are sent separately and never parsed as SQL.
        """
        if params is None:
            self._send(b"Q", encode_string(sql))
            return self._read_until_ready()
        # Prepare the unnamed statement and execute it, pipelined in one round trip.
        self._send(b"P", encode_string("") + encode_string(sql))
        self._send(b"B", encode_execute("", params))
        try:
            self._read_until_ready()
        except AtlasError as prepare_error:
            try:
                self._read_until_ready()
            except AtlasError:
                pass
            raise prepare_error
        return self._read_until_ready()

    def prepare(self, sql):
        self._statements = getattr(self, "_statements", 0) + 1
        name = f"py{self._statements}"
        self._send(b"P", encode_string(name) + encode_string(sql))
        param_count = self._read_until_ready(want_param_count=True)
        return PreparedStatement(self, name, param_count)

    def _read_until_ready(self, want_param_count=False):
        results, error, param_count = [], None, 0
        while True:
            kind, body = self._recv()
            r = Reader(body)
            if kind == b"R":
                ncols = r.u16()
                columns = []
                for _ in range(ncols):
                    columns.append(r.string())
                    r.u8()  # column type
                rows = [tuple(r.value() for _ in range(ncols)) for _ in range(r.u64())]
                results.append({"columns": columns, "rows": rows, "tag": r.string()})
            elif kind == b"C":
                results.append({"tag": r.string(), "affected": r.u64()})
            elif kind == b"p":
                param_count = r.u16()
            elif kind == b"E":
                error = self._error(body)
            elif kind == b"Z":
                self.in_transaction = body == b"T"
                if error:
                    raise error
                return param_count if want_param_count else results

    def close(self):
        self._send(b"X", b"")
        self.sock.close()


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5480
    db = Connection(host, port)
    print("Connected to", db.server_name)
    db.query("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)")
    db.query("DELETE FROM notes")
    # Parameters: the quote in the text is just data, not SQL.
    insert = db.prepare("INSERT INTO notes VALUES ($1, $2)")
    for i, body in enumerate(["hola", "desde Python", "it's safe; DROP TABLE notes"], start=1):
        insert.execute(i, body)
    insert.close()
    try:
        db.query("INSERT INTO notes VALUES (?, ?)", [1, "duplicate"])
    except AtlasError as e:
        print("Rejected as expected ->", e)
    for row in db.query("SELECT id, body FROM notes WHERE id >= ? ORDER BY id", [1])[0]["rows"]:
        print(row)
    db.close()
