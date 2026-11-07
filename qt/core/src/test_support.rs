use std::{
    path::Path,
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

// Reap subprocesses even when a test fails before its normal shutdown.
pub(crate) struct TestProcess(pub(crate) Child);
impl Drop for TestProcess {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

pub(crate) fn spawn_test(name: &str, dir: &Path, role: &str) -> TestProcess {
    TestProcess(
        Command::new(std::env::current_exe().unwrap())
            .args(["--exact", name, "--nocapture"])
            .env("ROOM237_TEST_DIR", dir)
            .env("ROOM237_TEST_ROLE", role)
            .stdout(Stdio::null())
            .spawn()
            .unwrap(),
    )
}

pub(crate) fn wait_for_file(path: &Path) {
    let started = Instant::now();
    while !path.exists() {
        assert!(
            started.elapsed() < Duration::from_secs(15),
            "Missing signal: {}",
            path.display()
        );
        thread::sleep(Duration::from_millis(5));
    }
}
