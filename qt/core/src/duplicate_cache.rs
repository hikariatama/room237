//! Fixed-capacity LRU pages, shared by hash verification and MIH postings.
use std::{collections::HashMap, hash::Hash};
struct Slot<K> {
    key: K,
    data: Vec<u64>,
    previous: Option<usize>,
    next: Option<usize>,
}
pub(super) struct Pages<K> {
    map: HashMap<K, usize>,
    slots: Vec<Slot<K>>,
    capacity: usize,
    head: Option<usize>,
    tail: Option<usize>,
}
impl<K: Copy + Eq + Hash> Pages<K> {
    pub fn new(capacity: usize) -> Self {
        assert!(capacity > 0);
        Self {
            map: HashMap::new(),
            slots: Vec::new(),
            capacity,
            head: None,
            tail: None,
        }
    }
    fn unlink(&mut self, index: usize) {
        let previous = self.slots[index].previous;
        let next = self.slots[index].next;
        if let Some(i) = previous {
            self.slots[i].next = next;
        } else {
            self.head = next;
        }
        if let Some(i) = next {
            self.slots[i].previous = previous;
        } else {
            self.tail = previous;
        }
    }
    fn front(&mut self, index: usize) {
        self.slots[index].previous = None;
        self.slots[index].next = self.head;
        if let Some(i) = self.head {
            self.slots[i].previous = Some(index);
        } else {
            self.tail = Some(index);
        }
        self.head = Some(index);
    }
    pub fn get(
        &mut self,
        key: K,
        load: impl FnOnce() -> Result<Vec<u64>, String>,
    ) -> Result<&[u64], String> {
        let index = if let Some(index) = self.map.get(&key).copied() {
            self.unlink(index);
            index
        } else {
            let values = load()?;
            let index = if self.slots.len() < self.capacity {
                self.slots.push(Slot {
                    key,
                    data: values,
                    previous: None,
                    next: None,
                });
                self.slots.len() - 1
            } else {
                let index = self.tail.unwrap();
                self.unlink(index);
                self.map.remove(&self.slots[index].key);
                self.slots[index] = Slot {
                    key,
                    data: values,
                    previous: None,
                    next: None,
                };
                index
            };
            self.map.insert(key, index);
            index
        };
        self.front(index);
        Ok(&self.slots[index].data)
    }
    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.slots.len()
    }
    #[cfg(test)]
    pub fn bytes(&self) -> usize {
        self.slots.iter().map(|p| p.data.len() * 8).sum()
    }
    #[cfg(test)]
    pub fn contains(&self, key: K) -> bool {
        self.map.contains_key(&key)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lru_moves_hits_and_evicts_the_oldest_page() {
        let mut cache = Pages::new(2);
        cache.get(1, || Ok(vec![1])).unwrap();
        cache.get(2, || Ok(vec![2])).unwrap();
        cache.get(1, || panic!("cache hit must not load")).unwrap();
        cache.get(3, || Ok(vec![3])).unwrap();
        assert!(cache.contains(1));
        assert!(!cache.contains(2));
        assert!(cache.contains(3));
        assert_eq!(cache.len(), 2);
        assert!(cache.get(4, || Err("failure".into())).is_err());
        assert!(cache.contains(1));
        assert_eq!(cache.bytes(), 16);
    }
}
