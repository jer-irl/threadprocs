use std::sync::atomic::{AtomicPtr, Ordering};
use std::ffi::c_void;

use crate::sys::{tproc_is_threadproc, tproc_registry};

const PTRS_LEN: usize = 4096 / std::mem::size_of::<*mut c_void>();

#[derive(Debug)]
pub struct Registry<T: 'static> {
	ptrs: &'static mut [*mut T; PTRS_LEN],
}

impl<T> Registry<T> {
	pub fn new() -> Option<Self> {
		let res = raw_registry().map(|reg| Self {
			ptrs: unsafe { &mut *(reg.as_ptr() as *mut [*mut T; PTRS_LEN]) },
		});
		res
	}

	pub fn get(&self, index: usize) -> Option<&'static T> {
		unsafe {
			self.ptrs.get(index)?.as_ref()
		}
	}

	pub fn try_set(&mut self, index: usize, value: T) -> Result<&'static T, &'static T> {
		let atomic_ptr = AtomicPtr::<c_void>::new(self.ptrs.get(index).unwrap().cast());
		let current = atomic_ptr.load(Ordering::Relaxed);
		if current != std::ptr::null_mut() {
			return Err(unsafe { &*(current as *const T) });
		}
		let boxed = Box::new(value);
		let raw_boxed = Box::into_raw(boxed);
		let res = atomic_ptr.compare_exchange(current, raw_boxed as *mut c_void, Ordering::AcqRel, Ordering::Acquire);
		match res {
			Ok(_prev) => Ok(unsafe { &*(raw_boxed as *const T) }),
			Err(existing) => {
				unsafe { let _ = Box::from_raw(raw_boxed); }
				Err(unsafe { &*(existing as *const T) })
			}
		}
	}
}

fn is_threadproc() -> bool {
	unsafe { tproc_is_threadproc() != 0 }
}

fn raw_registry() -> Option<&'static [u8]> {
	if !is_threadproc() {
		return None;
	}
	unsafe {
		let reg = tproc_registry();
		Some(std::slice::from_raw_parts(reg as *const u8, 4096usize))
	}
}
