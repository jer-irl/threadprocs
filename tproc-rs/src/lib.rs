mod registry;
pub mod shmring;

pub use registry::Registry;

#[allow(unused_variables)]
#[allow(dead_code)]
mod sys {
	include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}
