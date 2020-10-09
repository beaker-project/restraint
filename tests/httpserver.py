import argparse
import os
import signal
import sys

from six.moves import SimpleHTTPServer
from six.moves import socketserver


def sigterm_handler(signal, frame):
    raise SystemExit("received sigterm")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--port', help='port to listen on', default=8000)
    parser.add_argument('-i', '--pid', help='PID filename', default='httpserver.pid')
    parser.add_argument('--host', help='Network interface to bind to', default='127.0.0.1')

    args = parser.parse_args()

    server_address = (args.host, int(args.port))
    handler = SimpleHTTPServer.SimpleHTTPRequestHandler
    httpd = socketserver.TCPServer(
        server_address, handler, bind_and_activate=False
    )

    # Allow address reuse to avoid bind to fail due to TIME_WAIT
    # connections
    httpd.allow_reuse_address = True
    httpd.server_bind()
    httpd.server_activate()

    signal.signal(signal.SIGINT, sigterm_handler)
    signal.signal(signal.SIGTERM, sigterm_handler)

    pid = os.fork()
    if pid:
        # parent
        with open(args.pid, 'w') as f:
            f.write(str(pid))
        sys.exit(0)
    else:
        # child
        try:
            httpd.serve_forever()
        finally:
            os.unlink(args.pid)
        sys.exit(1)


if __name__ == '__main__':
    main()
