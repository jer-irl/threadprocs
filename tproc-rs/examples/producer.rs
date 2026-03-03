use tproc_rs::Registry;
use tproc_rs::shmring::SpscShmRing;

fn main() -> Result<(), Box<dyn std::error::Error>> {
	let mut registry = Registry::<SpscShmRing<u64>>::new().expect("Not running in a threadproc environment");
	let new_ring = SpscShmRing::new(1024);
	let ring = match registry.try_set(0, new_ring) {
		Ok(channel) => channel,
		Err(existing) => existing,
	};

	dbg!(registry);
	dbg!(ring);

	let sender = ring.get_sender();

	for i in 0..100 {
		println!("Sending {}", i);
		sender.send(i).unwrap();
	}

	dbg!(ring);

	Ok(())
}
