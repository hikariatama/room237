//! Test-only wall-clock accounting. No timers are compiled into the application.
use std::{
    cell::RefCell,
    collections::BTreeMap,
    time::{Duration, Instant},
};

#[derive(Default)]
struct State {
    enabled: bool,
    stack: Vec<Frame>,
    rows: BTreeMap<&'static str, Row>,
}
struct Frame {
    label: &'static str,
    start: Instant,
    children: Duration,
}
#[derive(Default)]
struct Row {
    calls: u64,
    inclusive: Duration,
    exclusive: Duration,
}
thread_local! {static STATE:RefCell<State>=RefCell::new(State::default());}

pub(super) struct Span(bool);
pub(super) fn span(label: &'static str) -> Span {
    let enabled = STATE.with(|state| {
        let mut state = state.borrow_mut();
        if !state.enabled {
            return false;
        }
        state.stack.push(Frame {
            label,
            start: Instant::now(),
            children: Duration::ZERO,
        });
        true
    });
    Span(enabled)
}
impl Drop for Span {
    fn drop(&mut self) {
        if !self.0 {
            return;
        }
        let end = Instant::now();
        STATE.with(|state| {
            let mut state = state.borrow_mut();
            let frame = state.stack.pop().expect("unbalanced profile span");
            let elapsed = end.duration_since(frame.start);
            let row = state.rows.entry(frame.label).or_default();
            row.calls += 1;
            row.inclusive += elapsed;
            row.exclusive += elapsed.saturating_sub(frame.children);
            if let Some(parent) = state.stack.last_mut() {
                parent.children += elapsed;
            }
        });
    }
}
pub(super) fn measure<T>(enabled: bool, f: impl FnOnce() -> T) -> (T, Option<serde_json::Value>) {
    if !enabled {
        return (f(), None);
    }
    STATE.with(|state| {
        let mut state = state.borrow_mut();
        assert!(!state.enabled);
        *state = State {
            enabled: true,
            ..Default::default()
        };
    });
    let start = Instant::now();
    let result = f();
    let elapsed = start.elapsed();
    let rows = STATE.with(|state| {
        let mut state = state.borrow_mut();
        assert!(state.stack.is_empty());
        state.enabled = false;
        std::mem::take(&mut state.rows)
    });
    let rows:Vec<_>=rows.into_iter().map(|(label,row)|serde_json::json!({"label":label,"calls":row.calls,"inclusive_ms":row.inclusive.as_secs_f64()*1000.0,"exclusive_ms":row.exclusive.as_secs_f64()*1000.0})).collect();
    (
        result,
        Some(serde_json::json!({"elapsed_ms":elapsed.as_secs_f64()*1000.0,"rows":rows})),
    )
}
