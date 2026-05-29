use std::env;
use std::io;
use std::mem;
use std::net::TcpListener;
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::net::UnixStream;
use std::thread;
use std::time::Duration;

const BACKENDS: [&str; 2] = ["/sockets/api1.sock", "/sockets/api2.sock"];

struct Upstream {
    path: &'static str,
    stream: UnixStream,
    byte: [u8; 1],
    control: [u8; 64],
}

impl Upstream {
    fn connect(path: &'static str) -> UnixStream {
        loop {
            match UnixStream::connect(path) {
                Ok(stream) => return stream,
                Err(_) => thread::sleep(Duration::from_millis(10)),
            }
        }
    }

    fn new(path: &'static str) -> Self {
        Self {
            path,
            stream: Self::connect(path),
            byte: [1],
            control: [0; 64],
        }
    }

    fn reconnect(&mut self) {
        self.stream = Self::connect(self.path);
    }

    fn send_fd(&mut self, fd: RawFd) -> io::Result<()> {
        for attempt in 0..2 {
            if send_fd_raw(self.stream.as_raw_fd(), fd, &mut self.byte, &mut self.control).is_ok() {
                return Ok(());
            }
            if attempt == 0 {
                self.reconnect();
            }
        }
        Err(io::Error::last_os_error())
    }
}

fn send_fd_raw(sock: RawFd, fd: RawFd, byte: &mut [u8; 1], control: &mut [u8; 64]) -> io::Result<()> {
    let mut iov = libc::iovec {
        iov_base: byte.as_mut_ptr().cast(),
        iov_len: 1,
    };
    control.fill(0);
    let mut msg: libc::msghdr = unsafe { mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = unsafe { libc::CMSG_SPACE(mem::size_of::<RawFd>() as _) as _ };
    unsafe {
        let cmsg = libc::CMSG_FIRSTHDR(&msg);
        (*cmsg).cmsg_level = libc::SOL_SOCKET;
        (*cmsg).cmsg_type = libc::SCM_RIGHTS;
        (*cmsg).cmsg_len = libc::CMSG_LEN(mem::size_of::<RawFd>() as _) as _;
        *(libc::CMSG_DATA(cmsg) as *mut RawFd) = fd;
        if libc::sendmsg(sock, &msg, libc::MSG_NOSIGNAL) < 0 {
            return Err(io::Error::last_os_error());
        }
    }
    Ok(())
}

fn main() -> io::Result<()> {
    let port = env::var("PORT").unwrap_or_else(|_| "9999".to_string());
    let listener = TcpListener::bind(format!("0.0.0.0:{port}"))?;
    listener.set_nonblocking(false)?;

    let mut upstreams = [Upstream::new(BACKENDS[0]), Upstream::new(BACKENDS[1])];
    let mut next = 0usize;
    eprintln!("rinha-fd-lb listening on :{port}");

    for client in listener.incoming() {
        let client = match client {
            Ok(client) => client,
            Err(err) => {
                eprintln!("accept error: {err}");
                continue;
            }
        };
        let _ = client.set_nodelay(true);
        let fd = client.as_raw_fd();
        let first = next & 1;
        next = next.wrapping_add(1);
        if upstreams[first].send_fd(fd).is_err() {
            let _ = upstreams[first ^ 1].send_fd(fd);
        }
    }
    Ok(())
}
