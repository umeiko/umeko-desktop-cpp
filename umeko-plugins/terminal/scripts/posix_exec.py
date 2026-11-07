"""Acquire a controlling terminal only after exec into a fresh interpreter."""
import fcntl
import os
import signal
import sys
import termios

error_fd=int(sys.argv[1])
os.set_inheritable(error_fd,False)
try:
    fcntl.ioctl(0,termios.TIOCSCTTY,0)
    for signum in (signal.SIGHUP,signal.SIGINT,signal.SIGQUIT,signal.SIGTERM,signal.SIGPIPE):
        signal.signal(signum,signal.SIG_DFL)
    os.execvpe(sys.argv[2],sys.argv[2:],os.environ)
except BaseException as error:
    os.write(error_fd,str(error).encode('utf-8','replace')[:4096])
    os._exit(127)
