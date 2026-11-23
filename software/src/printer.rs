//! Serial link with the firmware (see the protocol at the top of firmware/letratag/letratag.ino)

use std::io::{ErrorKind, Read, Write};
use std::sync::mpsc::{Receiver, Sender};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use serialport::{SerialPort, SerialPortType};

/// USB vendor IDs under which the arduino-pico core announces the boards:
/// Seeed for the XIAO RP2350 (rev B), Raspberry Pi for the Waveshare RP2350-Zero (V3)
const VIDS: [u16; 2] = [0x2886, 0x2E8A];

pub enum Event {
    Line(String),
    Disconnected(String),
}

/// Head geometry, as returned by the INFO command
#[derive(Clone, Copy, Debug)]
pub struct Head {
    /// First usable dot (1 = bit 0)
    pub first: u32,
    pub dots: u32,
    pub pitch_um: u32,
    pub max_lines: u32,
    /// 2: the firmware cuts by itself and chains the labels (V3); 0: cut by hand
    pub cut: u32,
}

impl Head {
    fn parse(reply: &str) -> Result<Self> {
        let field = |name: &str| -> Result<u32> {
            reply
                .split_whitespace()
                .find_map(|word| word.strip_prefix(name)?.strip_prefix('='))
                .ok_or_else(|| anyhow!("INFO reply without {name}: {reply}"))?
                .parse()
                .with_context(|| format!("INFO reply, field {name}"))
        };
        Ok(Self {
            first: field("first")?,
            dots: field("dots")?,
            pitch_um: field("pitch_um")?,
            max_lines: field("max_lines")?,
            cut: field("cut").unwrap_or(0),
        })
    }
}

pub fn find_port() -> Result<String> {
    serialport::available_ports()
        .context("listing serial ports")?
        .into_iter()
        .filter(|p| matches!(&p.port_type, SerialPortType::UsbPort(u) if VIDS.contains(&u.vid)))
        .map(|p| p.port_name)
        // on macOS each port exists as /dev/tty.* and /dev/cu.*: prefer cu
        .max_by_key(|name| name.contains("/cu."))
        .ok_or_else(|| anyhow!("no Letratag board (XIAO RP2350 or RP2350-Zero) found on USB (otherwise, use --port)"))
}

pub struct Printer {
    port: Box<dyn SerialPort>,
    pub name: String,
}

impl Printer {
    /// Opens the port; a reader thread sends each received line on `tx`
    pub fn open(name: String, tx: Sender<Event>) -> Result<Self> {
        let mut port = serialport::new(&name, 115_200)
            .timeout(Duration::from_millis(100))
            .open()
            .with_context(|| format!("opening {name}"))?;
        // without DTR, the firmware considers the port closed and sends nothing
        port.write_data_terminal_ready(true)?;

        let mut reader = port.try_clone()?;
        thread::spawn(move || {
            let mut buffer = Vec::new();
            let mut chunk = [0u8; 256];
            loop {
                match reader.read(&mut chunk) {
                    Ok(n) => {
                        buffer.extend_from_slice(&chunk[..n]);
                        while let Some(pos) = buffer.iter().position(|&b| b == b'\n') {
                            let line: Vec<u8> = buffer.drain(..=pos).collect();
                            let text = String::from_utf8_lossy(&line).trim().to_string();
                            if !text.is_empty() && tx.send(Event::Line(text)).is_err() {
                                return;
                            }
                        }
                    }
                    Err(e) if e.kind() == ErrorKind::TimedOut => {}
                    Err(e) => {
                        let _ = tx.send(Event::Disconnected(e.to_string()));
                        return;
                    }
                }
            }
        });

        Ok(Self { port, name })
    }

    /// Sends one or more commands separated by newlines
    pub fn send(&mut self, commands: &str) -> Result<()> {
        self.port.write_all(commands.as_bytes())?;
        self.port.write_all(b"\n")?;
        Ok(())
    }

    pub fn request_info(&mut self, rx: &Receiver<Event>) -> Result<Head> {
        self.send("INFO")?;
        let deadline = Instant::now() + Duration::from_secs(3);
        while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
            match rx.recv_timeout(remaining) {
                Ok(Event::Line(l)) if l.starts_with("INFO ") => return Head::parse(&l),
                Ok(Event::Line(_)) => {}
                Ok(Event::Disconnected(e)) => bail!("{}: {e}", self.name),
                Err(_) => break,
            }
        }
        bail!("{} does not answer INFO (firmware up to date?)", self.name)
    }
}

#[cfg(test)]
mod tests {
    use super::Head;

    #[test]
    fn parses_info() {
        let h = Head::parse("INFO first=2 dots=29 pitch_um=150 max_lines=4000").unwrap();
        assert_eq!((h.first, h.dots, h.pitch_um, h.max_lines, h.cut), (2, 29, 150, 4000, 0));
        let h = Head::parse("INFO first=1 dots=30 pitch_um=150 max_lines=4000 cut=2").unwrap();
        assert_eq!((h.first, h.dots, h.cut), (1, 30, 2));
        assert!(Head::parse("INFO first=2").is_err());
    }
}
