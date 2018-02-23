all: smartlaundry

smartlaundry: 
	pio run
	pio run -t buildfs

upload:
	pio run -t upload
	pio run -t uploadfs
