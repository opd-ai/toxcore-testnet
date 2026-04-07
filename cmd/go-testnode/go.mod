module go-testnode

go 1.25.0

// The replace directive is set at CI build time:
//   go mod edit -replace github.com/opd-ai/toxcore=../../vendor/opd-ai-toxcore
// It is intentionally absent here so the module validates without the vendor tree.

require github.com/opd-ai/toxcore v1.4.0-qtox-preview

require (
	github.com/cretz/bine v0.2.0 // indirect
	github.com/flynn/noise v1.1.0 // indirect
	github.com/go-i2p/i2pkeys v0.33.92 // indirect
	github.com/go-i2p/onramp v0.33.92 // indirect
	github.com/go-i2p/sam3 v0.33.92 // indirect
	github.com/klauspost/cpuid/v2 v2.3.0 // indirect
	github.com/klauspost/reedsolomon v1.13.3 // indirect
	github.com/opd-ai/magnum v0.0.0-20260324142352-b5664a8a5c6a // indirect
	github.com/opd-ai/vp8 v0.0.0-20260324160009-fb5eac94efed // indirect
	github.com/pion/randutil v0.1.0 // indirect
	github.com/pion/rtp v1.8.22 // indirect
	github.com/sirupsen/logrus v1.9.4 // indirect
	golang.org/x/crypto v0.48.0 // indirect
	golang.org/x/image v0.38.0 // indirect
	golang.org/x/net v0.50.0 // indirect
	golang.org/x/sys v0.41.0 // indirect
)
