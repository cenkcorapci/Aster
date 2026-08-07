// Package aster is the Go client for Aster, the peer-to-peer vector
// database. The protocol is defined in //aster/rpc/aster.thrift; the
// transport and generated code land in milestone M5.
//
//	client, err := aster.Connect(aster.Options{Seeds: []string{"10.0.0.1:7000"}})
//	products := client.Collection("products")
//	err = products.Upsert(ctx, "doc-1", vec, aster.WithTags("electronics"))
//	hits, err := products.Search(ctx, query, aster.TopK(10), aster.EfSearch(128))
package aster

import (
	"context"
	"errors"
)

var errNotImplemented = errors.New("aster: transport lands in milestone M5")

// Options configures the client. Any node can coordinate, so Seeds is only
// used for discovery; the client learns the full ring via gossip metadata.
type Options struct {
	Seeds     []string
	TLS       bool
	TimeoutMs int
}

// Hit is a single search result. Higher score is better for all metrics.
type Hit struct {
	ID       string
	Score    float64
	Metadata []byte
}

// Client is safe for concurrent use.
type Client struct {
	opts Options
}

func Connect(opts Options) (*Client, error) {
	return &Client{opts: opts}, nil
}

// Collection returns a handle; it performs no I/O.
func (c *Client) Collection(name string) *Collection {
	return &Collection{client: c, name: name}
}

type Collection struct {
	client *Client
	name   string
}

func (c *Collection) Upsert(ctx context.Context, id string, vector []float32, opts ...CallOption) error {
	return errNotImplemented
}

func (c *Collection) Delete(ctx context.Context, id string, opts ...CallOption) error {
	return errNotImplemented
}

func (c *Collection) Search(ctx context.Context, vector []float32, opts ...CallOption) ([]Hit, error) {
	return nil, errNotImplemented
}

// CallOption tunes a single request (top-k, ef_search, tags, consistency).
type CallOption func(*callConfig)

type callConfig struct {
	topK        int
	efSearch    int
	tags        []string
	consistency string
}

func TopK(k int) CallOption           { return func(c *callConfig) { c.topK = k } }
func EfSearch(ef int) CallOption      { return func(c *callConfig) { c.efSearch = ef } }
func WithTags(t ...string) CallOption { return func(c *callConfig) { c.tags = t } }
func Consistency(l string) CallOption { return func(c *callConfig) { c.consistency = l } }
