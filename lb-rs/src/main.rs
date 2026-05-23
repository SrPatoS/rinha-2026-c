use std::env;
use std::io;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Mutex;

const BUFFER_CAP: usize = 32 * 1024;
const BACKENDS: [&str; 2] = ["api1:8080", "api2:8080"];

struct ConnBuffer {
    data: Vec<u8>,
}

impl ConnBuffer {
    fn new() -> Self {
        Self { data: Vec::with_capacity(BUFFER_CAP) }
    }

    async fn read_http_message(&mut self, stream: &mut TcpStream) -> io::Result<Vec<u8>> {
        loop {
            if let Some(header_end) = find_headers_end(&self.data) {
                let body_len = content_length(&self.data[..header_end]).unwrap_or(0);
                let total = header_end + body_len;
                while self.data.len() < total {
                    self.read_more(stream).await?;
                }
                let message: Vec<u8> = self.data.drain(..total).collect();
                return Ok(message);
            }
            self.read_more(stream).await?;
        }
    }

    async fn read_more(&mut self, stream: &mut TcpStream) -> io::Result<()> {
        if self.data.len() >= BUFFER_CAP {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "message too large"));
        }
        let mut chunk = [0u8; 4096];
        let n = stream.read(&mut chunk).await?;
        if n == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "closed"));
        }
        self.data.extend_from_slice(&chunk[..n]);
        Ok(())
    }
}

struct BackendConn {
    addr: &'static str,
    stream: Option<TcpStream>,
    buffer: ConnBuffer,
}

impl BackendConn {
    fn new(addr: &'static str) -> Self {
        Self { addr, stream: None, buffer: ConnBuffer::new() }
    }

    async fn send_request(&mut self, request: &[u8]) -> io::Result<Vec<u8>> {
        for _ in 0..2 {
            if self.stream.is_none() {
                let stream = TcpStream::connect(self.addr).await?;
                stream.set_nodelay(true)?;
                self.stream = Some(stream);
                self.buffer.data.clear();
            }

            let stream = self.stream.as_mut().unwrap();
            if stream.write_all(request).await.is_err() {
                self.stream = None;
                continue;
            }

            match self.buffer.read_http_message(stream).await {
                Ok(response) => return Ok(response),
                Err(_) => {
                    self.stream = None;
                    self.buffer.data.clear();
                }
            }
        }
        Err(io::Error::new(io::ErrorKind::BrokenPipe, "backend failed"))
    }
}

struct State {
    pool: Vec<Arc<Mutex<BackendConn>>>,
    next: AtomicUsize,
}

fn find_headers_end(data: &[u8]) -> Option<usize> {
    data.windows(4).position(|w| w == b"\r\n\r\n").map(|i| i + 4)
}

fn content_length(headers: &[u8]) -> Option<usize> {
    for line in headers.split(|b| *b == b'\n') {
        let line = trim_cr(line);
        if line.len() >= 15 && eq_ignore_ascii_case(&line[..15], b"content-length:") {
            let value = std::str::from_utf8(&line[15..]).ok()?.trim();
            return value.parse().ok();
        }
    }
    None
}

fn trim_cr(line: &[u8]) -> &[u8] {
    line.strip_suffix(b"\r").unwrap_or(line)
}

fn eq_ignore_ascii_case(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
}

async fn handle_client(mut client: TcpStream, state: Arc<State>) {
    let _ = client.set_nodelay(true);
    let mut client_buffer = ConnBuffer::new();

    while let Ok(request) = client_buffer.read_http_message(&mut client).await {
        let index = state.next.fetch_add(1, Ordering::Relaxed) % state.pool.len();
        let response = {
            let mut backend = state.pool[index].lock().await;
            backend.send_request(&request).await
        };

        match response {
            Ok(bytes) => {
                if client.write_all(&bytes).await.is_err() {
                    break;
                }
            }
            Err(_) => break,
        }
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() -> io::Result<()> {
    let port = env::var("PORT").unwrap_or_else(|_| "9999".to_string());
    let pool_per_backend = env::var("BACKEND_POOL")
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(24)
        .max(1);

    let mut pool = Vec::with_capacity(pool_per_backend * BACKENDS.len());
    for _ in 0..pool_per_backend {
        for addr in BACKENDS {
            pool.push(Arc::new(Mutex::new(BackendConn::new(addr))));
        }
    }

    let state = Arc::new(State { pool, next: AtomicUsize::new(0) });
    let listener = TcpListener::bind(format!("0.0.0.0:{port}")).await?;
    eprintln!("rinha-lb-rs listening on :{port} with {} backend connections", state.pool.len());

    loop {
        let (client, _) = listener.accept().await?;
        tokio::spawn(handle_client(client, state.clone()));
    }
}
