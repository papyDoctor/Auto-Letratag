//! Registry of saved templates: a text file, one template per line.
//! Empty lines and lines starting with # are ignored; the file can be
//! edited by hand.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

const HEADER: &str = "\
# Letratag label templates, one per line.
# {n} or {n:03} = number in the series, | = new line.
";

/// Name of the templates file in older versions, still read if `templates.txt` is missing
const LEGACY_FILE_NAME: &str = "modeles.txt";

pub struct Registry {
    pub path: PathBuf,
    pub templates: Vec<String>,
}

impl Registry {
    /// ~/.config/letratag/templates.txt
    pub fn default_path() -> PathBuf {
        let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default();
        home.join(".config/letratag/templates.txt")
    }

    /// Reads the registry; a missing file gives an empty registry. If `path` does not exist
    /// but the older `modeles.txt` does in the same folder, the templates are read from it
    /// (they are written to `path` on the next save).
    pub fn load(path: PathBuf) -> Result<Self> {
        let templates = match Self::read(&path)? {
            Some(templates) => templates,
            None => match path.parent().map(|folder| folder.join(LEGACY_FILE_NAME)) {
                Some(legacy) if legacy != path => Self::read(&legacy)?.unwrap_or_default(),
                _ => Vec::new(),
            },
        };
        Ok(Self { path, templates })
    }

    /// Templates in the file at `path`, or None if it does not exist
    fn read(path: &Path) -> Result<Option<Vec<String>>> {
        match std::fs::read_to_string(path) {
            Ok(text) => Ok(Some(
                text.lines()
                    .map(str::trim_end)
                    .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
                    .map(String::from)
                    .collect(),
            )),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
        }
    }

    fn save(&self) -> Result<()> {
        if let Some(folder) = self.path.parent() {
            std::fs::create_dir_all(folder)
                .with_context(|| format!("creating {}", folder.display()))?;
        }
        let mut text = HEADER.to_string();
        for t in &self.templates {
            text.push_str(t);
            text.push('\n');
        }
        std::fs::write(&self.path, text)
            .with_context(|| format!("writing {}", self.path.display()))
    }

    /// Adds a template if it is not already there; true if it was added
    pub fn add(&mut self, template: &str) -> Result<bool> {
        let template = template.trim();
        if template.is_empty() || self.templates.iter().any(|t| t == template) {
            return Ok(false);
        }
        self.templates.push(template.to_string());
        self.save()?;
        Ok(true)
    }

    pub fn remove(&mut self, index: usize) -> Result<()> {
        if index < self.templates.len() {
            self.templates.remove(index);
            self.save()?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::Registry;

    #[test]
    fn adds_reloads_removes() {
        let folder = std::env::temp_dir().join(format!("letratag-test-{}", std::process::id()));
        let path = folder.join("templates.txt");
        let mut r = Registry::load(path.clone()).unwrap();
        assert!(r.templates.is_empty());
        assert!(r.add("BOX-{n:03}|Kitchen").unwrap());
        assert!(r.add("ODO01{n:06}").unwrap());
        assert!(!r.add("ODO01{n:06}").unwrap(), "duplicate");
        let mut r = Registry::load(path.clone()).unwrap();
        assert_eq!(r.templates, ["BOX-{n:03}|Kitchen", "ODO01{n:06}"]);
        r.remove(0).unwrap();
        let r = Registry::load(path).unwrap();
        assert_eq!(r.templates, ["ODO01{n:06}"]);
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn reads_legacy_file() {
        let folder =
            std::env::temp_dir().join(format!("letratag-test-legacy-{}", std::process::id()));
        std::fs::create_dir_all(&folder).unwrap();
        std::fs::write(folder.join("modeles.txt"), "# header\nODO01{n:06}\n").unwrap();
        let path = folder.join("templates.txt");
        let mut r = Registry::load(path.clone()).unwrap();
        assert_eq!(r.templates, ["ODO01{n:06}"]);
        assert!(r.add("BOX-{n:03}").unwrap());
        let r = Registry::load(path.clone()).unwrap();
        assert_eq!(r.templates, ["ODO01{n:06}", "BOX-{n:03}"]);
        assert!(path.exists());
        std::fs::remove_dir_all(folder).unwrap();
    }
}
