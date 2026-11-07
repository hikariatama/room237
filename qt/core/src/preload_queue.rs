use std::{
    cmp::Reverse,
    collections::{BTreeMap, HashMap},
    path::{Path, PathBuf},
};

use crate::preload::TaskPriority;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum QueueKey {
    High(Reverse<u64>),
    Low(u64),
}

/// Pending tasks indexed by path and scheduling order. Promotion does not scan
/// the backlog or leave stale queue entries behind. High priority retains the
/// existing front-insertion order; background tasks retain FIFO order.
pub(super) struct PendingTasks<T> {
    tasks: HashMap<PathBuf, (QueueKey, T)>,
    order: BTreeMap<QueueKey, PathBuf>,
    sequence: u64,
}

impl<T> Default for PendingTasks<T> {
    fn default() -> Self {
        Self {
            tasks: HashMap::new(),
            order: BTreeMap::new(),
            sequence: 0,
        }
    }
}

impl<T> PendingTasks<T> {
    fn next_key(&mut self, priority: TaskPriority) -> QueueKey {
        if self.sequence == u64::MAX {
            let count = self.tasks.len() as u64;
            let previous = std::mem::take(&mut self.order);
            for (index, (key, path)) in previous.into_iter().enumerate() {
                let key = match key {
                    QueueKey::High(_) => QueueKey::High(Reverse(count - index as u64)),
                    QueueKey::Low(_) => QueueKey::Low(index as u64),
                };
                self.tasks.get_mut(&path).unwrap().0 = key;
                self.order.insert(key, path);
            }
            self.sequence = count + 1;
        }
        let sequence = self.sequence;
        self.sequence += 1;
        match priority {
            TaskPriority::High => QueueKey::High(Reverse(sequence)),
            TaskPriority::Low => QueueKey::Low(sequence),
        }
    }

    /// Returns true for newly admitted or promoted work. Duplicate requests keep
    /// the admitted task's data and do not reorder an already high-priority task.
    pub(super) fn enqueue(&mut self, path: PathBuf, data: T, priority: TaskPriority) -> bool {
        if let Some((key, _)) = self.tasks.get(&path) {
            if !matches!((key, priority), (QueueKey::Low(_), TaskPriority::High)) {
                return false;
            }
            let key = self.next_key(priority);
            let (previous, _) = self.tasks.get_mut(&path).unwrap();
            self.order.remove(previous);
            *previous = key;
            self.order.insert(key, path);
        } else {
            let key = self.next_key(priority);
            self.order.insert(key, path.clone());
            self.tasks.insert(path, (key, data));
        }
        true
    }

    pub(super) fn pop_front(&mut self) -> Option<(PathBuf, T)> {
        let (_, path) = self.order.pop_first()?;
        let (_, data) = self.tasks.remove(&path).unwrap();
        Some((path, data))
    }

    pub(super) fn contains(&self, path: &Path) -> bool {
        self.tasks.contains_key(path)
    }

    pub(super) fn len(&self) -> usize {
        self.tasks.len()
    }

    pub(super) fn is_empty(&self) -> bool {
        self.tasks.is_empty()
    }

    pub(super) fn trim_prefix(&mut self, prefix: &Path) -> usize {
        let before = self.len();
        self.tasks.retain(|path, _| !path.starts_with(prefix));
        self.order.retain(|_, path| !path.starts_with(prefix));
        before - self.len()
    }

    pub(super) fn clear(&mut self) {
        self.tasks.clear();
        self.order.clear();
        self.sequence = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mixed_operations_match_front_insertion_and_background_fifo() {
        let mut queue = PendingTasks::default();
        let mut reference: Vec<(PathBuf, u32, TaskPriority)> = Vec::new();
        let mut random = 713u64;
        for step in 0..20_000u32 {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            let path = PathBuf::from(format!("/library/{}/{}", random % 4, random % 257));
            match (random >> 12) % 100 {
                0..=72 => {
                    let priority = if random & 1 == 0 {
                        TaskPriority::High
                    } else {
                        TaskPriority::Low
                    };
                    let position = reference.iter().position(|(p, _, _)| p == &path);
                    let mut changed = false;
                    if let Some(position) = position {
                        if matches!(
                            (reference[position].2, priority),
                            (TaskPriority::Low, TaskPriority::High)
                        ) {
                            let (path, data, _) = reference.remove(position);
                            reference.insert(0, (path, data, priority));
                            changed = true;
                        }
                    } else {
                        match priority {
                            TaskPriority::High => {
                                reference.insert(0, (path.clone(), step, priority))
                            }
                            TaskPriority::Low => reference.push((path.clone(), step, priority)),
                        }
                        changed = true;
                    }
                    assert_eq!(queue.enqueue(path, step, priority), changed);
                }
                73..=94 => {
                    let expected = if reference.is_empty() {
                        None
                    } else {
                        let (path, data, _) = reference.remove(0);
                        Some((path, data))
                    };
                    assert_eq!(queue.pop_front(), expected);
                }
                95..=98 => {
                    let prefix = PathBuf::from(format!("/library/{}", random % 4));
                    let before = reference.len();
                    reference.retain(|(path, _, _)| !path.starts_with(&prefix));
                    assert_eq!(queue.trim_prefix(&prefix), before - reference.len());
                }
                _ => {
                    reference.clear();
                    queue.clear();
                }
            }
            assert_eq!(queue.len(), reference.len());
        }
        for (path, data, _) in reference {
            assert_eq!(queue.pop_front(), Some((path, data)));
        }
        assert!(queue.pop_front().is_none());
    }

    #[test]
    #[ignore = "paired indexed versus linear promotion queue microbenchmark"]
    fn benchmark_backlog_promotion() {
        use std::{collections::VecDeque, time::Instant};
        let paths: Vec<_> = (0..10_000)
            .map(|i| PathBuf::from(format!("/background/{i}")))
            .collect();
        for sample in 0..3 {
            let start = Instant::now();
            let mut linear: VecDeque<_> = paths
                .iter()
                .cloned()
                .enumerate()
                .map(|(i, p)| (p, i))
                .collect();
            let linear_enqueue = start.elapsed();
            let start = Instant::now();
            for path in paths.iter().rev().take(1000) {
                let position = linear.iter().position(|(p, _)| p == path).unwrap();
                let task = linear.remove(position).unwrap();
                linear.push_front(task);
            }
            let linear_promote = start.elapsed();
            let start = Instant::now();
            let mut expected = Vec::with_capacity(paths.len());
            while let Some(task) = linear.pop_front() {
                expected.push(task);
            }
            let linear_drain = start.elapsed();
            let start = Instant::now();
            let mut indexed = PendingTasks::default();
            for (i, path) in paths.iter().enumerate() {
                indexed.enqueue(path.clone(), i, TaskPriority::Low);
            }
            let indexed_enqueue = start.elapsed();
            let start = Instant::now();
            for path in paths.iter().rev().take(1000) {
                assert!(indexed.enqueue(path.clone(), usize::MAX, TaskPriority::High));
            }
            let indexed_promote = start.elapsed();
            let start = Instant::now();
            let mut actual = Vec::with_capacity(paths.len());
            while let Some(task) = indexed.pop_front() {
                actual.push(task);
            }
            let indexed_drain = start.elapsed();
            assert_eq!(actual, expected);
            assert!(indexed.is_empty());
            eprintln!(
                "{}",
                serde_json::json!({
                    "sample": sample, "backlog": paths.len(), "promotions": 1000,
                    "linearNanos": {"enqueue": linear_enqueue.as_nanos(), "promote": linear_promote.as_nanos(), "drain": linear_drain.as_nanos()},
                    "indexedNanos": {"enqueue": indexed_enqueue.as_nanos(), "promote": indexed_promote.as_nanos(), "drain": indexed_drain.as_nanos()},
                    "parity": true,
                })
            );
        }
    }

    #[test]
    fn sequence_rollover_preserves_order_and_promotion_data() {
        let mut queue = PendingTasks::default();
        for (path, priority) in [
            ("a", TaskPriority::Low),
            ("b", TaskPriority::Low),
            ("c", TaskPriority::High),
            ("d", TaskPriority::High),
        ] {
            queue.enqueue(PathBuf::from(path), path, priority);
        }
        queue.sequence = u64::MAX;
        assert!(queue.enqueue(PathBuf::from("b"), "discarded", TaskPriority::High));
        for expected in ["b", "d", "c", "a"] {
            assert_eq!(queue.pop_front(), Some((PathBuf::from(expected), expected)));
        }
        assert!(queue.is_empty());
    }

    #[test]
    fn trimming_and_reenqueue_leave_no_old_positions_or_tasks() {
        let mut queue = PendingTasks::default();
        for path in ["/old/a", "/keep/a", "/old/b", "/keep/b"] {
            queue.enqueue(PathBuf::from(path), 1, TaskPriority::Low);
        }
        queue.enqueue(PathBuf::from("/old/a"), 2, TaskPriority::High);
        assert_eq!(queue.trim_prefix(Path::new("/old")), 2);
        assert!(!queue.contains(Path::new("/old/a")));
        queue.enqueue(PathBuf::from("/old/a"), 3, TaskPriority::Low);
        for (expected, data) in [("/keep/a", 1), ("/keep/b", 1), ("/old/a", 3)] {
            assert_eq!(queue.pop_front(), Some((PathBuf::from(expected), data)));
        }
        assert!(queue.pop_front().is_none());
        queue.enqueue(PathBuf::from("/keep/a"), 4, TaskPriority::High);
        queue.clear();
        assert!(!queue.contains(Path::new("/keep/a")));
        queue.enqueue(PathBuf::from("/keep/a"), 5, TaskPriority::Low);
        assert_eq!(queue.pop_front(), Some((PathBuf::from("/keep/a"), 5)));
    }
}
