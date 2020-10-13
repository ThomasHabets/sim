package sim

import (
	"net"
	"sync"

	"github.com/fsnotify/fsnotify"
	log "github.com/sirupsen/logrus"
)

// MultiWatcher allows one fsnotify.Watcher to notify any number of
// clients.
type MultiWatcher struct {
	watcher *fsnotify.Watcher
	mu      sync.Mutex
	n       int64
	clients map[int64]chan fsnotify.Event
}

func NewWatcher(dir string) *MultiWatcher {
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		log.Fatalf("Failed to create fsnotify: %v", err)
	}
	if err := watcher.Add(dir); err != nil {
		log.Fatalf("Failed to start watching %q: %v", dir, err)
	}
	return &MultiWatcher{
		watcher: watcher,
		clients: make(map[int64]chan fsnotify.Event),
	}
}

// Close closes the watcher.
func (w *MultiWatcher) Close() {
	w.watcher.Close()
}

func (w *MultiWatcher) Len() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return len(w.clients)
}

// Run runs the loop that fans out all the notifications.
func (w *MultiWatcher) Run() {
	//case err, ok := <-watcher.Errors:
	//			log.Errorf("Watcher error: %v", err)
	//			if !ok {
	//				return
	//			}
	for {
		e := <-w.watcher.Events
		w.mu.Lock()
		for _, c := range w.clients {
			select {
			case c <- e:
			default:
			}
		}
		w.mu.Unlock()
	}
}

// Add adds a new client, returning a cancellation callback and the channel.
func (w *MultiWatcher) Add() (func(), <-chan fsnotify.Event) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.n++
	w.clients[w.n] = make(chan fsnotify.Event, 10)
	n := w.n
	return func() { w.remove(n) }, w.clients[w.n]
}

func (w *MultiWatcher) remove(n int64) {
	w.mu.Lock()
	defer w.mu.Unlock()
	close(w.clients[n])
	delete(w.clients, n)
}

func ReadRequest(full string) ([]byte, error) {
	c, err := net.Dial("unixpacket", full)
	if err != nil {
		return nil, err
	}
	defer c.Close()
	buf := make([]byte, 1000000)
	n, err := c.Read(buf)
	if err != nil {
		return nil, err
	}
	return buf[0:n], nil
}

func Reply(full string, data []byte) error {
	c, err := net.Dial("unixpacket", full)
	if err != nil {
		return err
	}
	defer c.Close()
	buf := make([]byte, 1000000)
	if _, err := c.Read(buf); err != nil {
		return err
	}
	_, err = c.Write(data)
	return err
}
