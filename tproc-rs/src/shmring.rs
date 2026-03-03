use std::sync::atomic::AtomicU64;
use std::mem::MaybeUninit;
use std::cell::UnsafeCell;

const CACHE_LINE_SIZE: usize = 64;
const PAGE_SIZE: usize = 4096;

#[repr(align(64))]
struct Padded<T> {
	value: T,
}

type RingElement<T> = Padded<UnsafeCell<MaybeUninit<T>>>;

#[derive(Debug)]
pub struct SpscShmRing<T> {
	ring: *mut [RingElement<T>],
	size: usize,
	head: AtomicU64,
	tail: AtomicU64,
	index_mask: u64,
}

fn lcm(a: usize, b: usize) -> usize {
	a * b / gcd(a, b)
}

fn gcd(a: usize, b: usize) -> usize {
	if b == 0 {
		a
	} else {
		gcd(b, a % b)
	}
}

impl<T> SpscShmRing<T> {
	pub fn new(min_size: usize) -> Self {
		let stride = std::mem::size_of::<RingElement<T>>();
		let size_bytes = lcm(stride, PAGE_SIZE);
		let size_bytes = if size_bytes > PAGE_SIZE { size_bytes } else { PAGE_SIZE * 2 };
		let mut size = size_bytes / stride;
		while size < min_size {
			size *= 2;
		}
		let ring = unsafe {std::alloc::alloc_zeroed(std::alloc::Layout::from_size_align(size_bytes, CACHE_LINE_SIZE).unwrap())} as *mut RingElement<T>;
		let ring = unsafe { std::slice::from_raw_parts_mut(ring, size) };

		Self {
			ring,
			size,
			head: AtomicU64::new(0),
			tail: AtomicU64::new(0),
			index_mask: (size as u64) - 1,
		}
	}

	pub fn get_sender<'a>(&'a self) -> Sender<'a, T> {
		Sender::new(self)
	}

	pub fn get_receiver<'a>(&'a self) -> Receiver<'a, T> {
		Receiver::new(self)
	}
}

pub struct Sender<'a, T> {
	ring: &'a SpscShmRing<T>,
}

impl<'a, T> Sender<'a, T> {
	pub fn new(ring: &'a SpscShmRing<T>) -> Self {
		Self { ring }
	}

	pub fn send(&self, value: T) -> Result<(), T> {

		let head = self.ring.head.load(std::sync::atomic::Ordering::Acquire);
		let tail = self.ring.tail.load(std::sync::atomic::Ordering::Relaxed);
		if tail - head == self.ring.size as u64 {
			return Err(value);
		}
		let index = tail & self.ring.index_mask;
		unsafe {
			let padded_ref = &mut (*self.ring.ring)[index as usize];
			padded_ref.value.get_mut().write(value);
		}
		self.ring.tail.store(tail + 1, std::sync::atomic::Ordering::Release);
		Ok(())
	}
}

pub struct Receiver<'a, T> {
	ring: &'a SpscShmRing<T>,
}

impl<'a, T> Receiver<'a, T> {
	pub fn new(ring: &'a SpscShmRing<T>) -> Self {
		Self { ring }
	}

	pub fn recv(&self) -> Option<T> {
		let head = self.ring.head.load(std::sync::atomic::Ordering::Acquire);
		let tail = self.ring.tail.load(std::sync::atomic::Ordering::Relaxed);
		if head == tail {
			return None;
		}
		let index = head & self.ring.index_mask;
		let value = unsafe {
			let r = &(*self.ring.ring)[index as usize].value;
			std::ptr::read(r.get()).assume_init()
		};
		self.ring.head.store(head + 1, std::sync::atomic::Ordering::Release);
		Some(value)
	}
}
