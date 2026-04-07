module go-testnode

go 1.22

// The replace directive is set at CI build time:
//   go mod edit -replace github.com/opd-ai/toxcore=../../vendor/opd-ai-toxcore
// It is intentionally absent here so the module validates without the vendor tree.
