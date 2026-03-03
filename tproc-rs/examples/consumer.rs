use tproc_rs::Registry;
use tproc_rs::shmring::SpscShmRing;

fn main() -> Result<(), Box<dyn std::error::Error>> {
	let mut registry = Registry::<SpscShmRing<u64>>::new().expect("Not running in a threadproc environment");
	let new_ring = SpscShmRing::new(1024);
	let ring = match registry.try_set(0, new_ring) {
		Ok(r) => r,
		Err(r) => r,
	};

	dbg!(ring);
	dbg!(registry);

	let receiver = ring.get_receiver();


	let mut count = 0;
	while count < 100 {
		if let Some(i) = receiver.recv() {
			println!("Received {}", i);
			count += 1;
		}
	}

	Ok(())
}
