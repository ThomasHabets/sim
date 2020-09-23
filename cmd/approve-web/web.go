/*
 *    Copyright 2020 Google LLC
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        https://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
package main

// This is a test web interface to talk to the app. It's a proof of
// concept at this point, and does not:
// * check the proto (or even parse the proto)
// * check the uid of the other end
// * take an external config or even flags.
//
// Basically:
// ==========================================================
// UGLY HACK AT THIS POINT DO NOT USE.
// ==========================================================
//
// Because of HTTPS requires an nginx config like:
// location /sim {
//     proxy_read_timeout 300;
//     proxy_pass  http://127.0.0.1:12345;
//     proxy_http_version 1.1;
//     proxy_set_header Upgrade $http_upgrade;
//     proxy_set_header Connection $connection_upgrade;
// }

import (
	"flag"
	"io/ioutil"
	"net"
	"net/http"
	"os"
	"path"
	"time"

	"github.com/fsnotify/fsnotify"
	"github.com/gorilla/mux"
	"github.com/gorilla/websocket"
	log "github.com/sirupsen/logrus"

	"github.com/ThomasHabets/sim/pkg/sim"
)

var (
	sockDir = "/var/run/sim"

	pin string

	watcher *sim.MultiWatcher

	upgrader = websocket.Upgrader{
		ReadBufferSize:  1024,
		WriteBufferSize: 1024,
		CheckOrigin: func(r *http.Request) bool {
			return true
		},
	}
)

func processGetOne(w http.ResponseWriter, r *http.Request, fn string) error {
	data, err := sim.ReadRequest(path.Join(sockDir, fn))
	if err != nil {
		return err
	}
	_, err = w.Write(data)
	return err
}

func handleGetOne(w http.ResponseWriter, r *http.Request) {
	id := mux.Vars(r)["id"]
	if err := processGetOne(w, r, id); err != nil {
		log.Errorf("Failed to get one: %v", err)
		w.WriteHeader(400)
	}
}

func handleGet(w http.ResponseWriter, r *http.Request) {
	files, err := ioutil.ReadDir(sockDir)
	if err != nil {
		log.Fatal(err)
	}
	for _, f := range files {
		w.Header().Set("x-sim-request", f.Name())
		if err := processGetOne(w, r, f.Name()); err != nil {
			w.WriteHeader(503)
			log.Fatal(err)
		}
		return
	}
	w.WriteHeader(404)
}

func handleApprove(w http.ResponseWriter, r *http.Request) {
	data, err := ioutil.ReadAll(r.Body)
	if err != nil {
		w.WriteHeader(400)
		log.Print(err)
		return
	}
	id := mux.Vars(r)["id"]
	fn := path.Join(sockDir, id)

	c, err := net.Dial("unixpacket", fn)
	if err != nil {
		w.WriteHeader(500)
		log.Print(err)
		return
	}
	defer c.Close()
	buf := make([]byte, 1000000)

	if _, err := c.Read(buf); err != nil {
		log.Print(err)
		w.WriteHeader(500)
		return
	}
	// ignore data
	if _, err := c.Write(data); err != nil {
		w.WriteHeader(500)
		log.Print(err)
		return
	}
	w.Write([]byte("OK"))

}

func handleStream(w http.ResponseWriter, r *http.Request) {

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println(err)
		return
	}
	defer conn.Close()

	if err := conn.SetReadDeadline(time.Now().Add(10 * time.Minute)); err != nil {
		log.Errorf("Setting read deadline: %v", err)
		return
	}

	// Start watcher first.
	cancel, ch := watcher.Add()
	defer cancel()
	log.Infof("Watching (active: %d)", watcher.Len())

	// Do first batch.
	seen := map[string]bool{}
	files, err := ioutil.ReadDir(sockDir)
	if err != nil {
		log.Errorf("Failed to list socket dir: %v", err)
		return
	}
	for _, f := range files {
		seen[f.Name()] = true
		log.Infof("Pre-sending %q", f.Name())
		if err := conn.WriteMessage(websocket.TextMessage, []byte(f.Name())); err != nil {
			log.Warningf("Failed to write message: %v", err)
			return
		}
	}

	defer log.Infof("End watching…")
	done := make(chan struct{})
	go func() {
		defer close(done)
		// Just to trigger the close handler.
		defer log.Debugf("Close handler exits")

		_, _, err := conn.ReadMessage()
		log.Debugf("ReadMessage returned: %v", err)
	}()

	for {
		select {
		case <-r.Context().Done():
			return
		case <-done:
			return
		case event, ok := <-ch:
			if event.Op != fsnotify.Create {
				break
			}
			log.Infof("Picked it up")
			if !ok {
				return
			}
			name := path.Base(event.Name)
			if seen[name] {
				continue
			}
			seen[name] = true
			log.Infof("Sending %q", name)
			if err := conn.WriteMessage(websocket.TextMessage, []byte(name)); err != nil {
				log.Warningf("Failed to write message: %v", err)
				return
			}
		}
		if len(seen) > 10000 {
			log.Warningf("Seen too many files, breaking connection")
			return
		}
	}
}

func checkPIN(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := r.Header.Get("x-sim-pin")
		if p != pin {
			log.Warningf("Wrong PIN sent from client. Denying")
			w.WriteHeader(403)
			return
		}
		h.ServeHTTP(w, r)
	})
}

func main() {
	flag.Parse()

	pin = os.Getenv("SIM_PIN")
	if pin == "" {
		log.Fatalf("Environment SIM_PIN required.")
	}

	// Set up watcher.
	watcher = sim.NewWatcher(sockDir)
	go watcher.Run()
	r := mux.NewRouter()
	r.HandleFunc("/get", handleGet)
	r.HandleFunc("/sim/get", handleGet)
	r.HandleFunc("/sim/get/{id:[A-Z0-9]+}", handleGetOne)
	r.HandleFunc("/approve/{id:[A-Z0-9]+}", handleApprove)
	r.HandleFunc("/sim/approve/{id:[A-Z0-9]+}", handleApprove)
	r.HandleFunc("/stream", handleStream)
	r.HandleFunc("/sim/stream", handleStream)
	srv := &http.Server{
		Handler: checkPIN(r),
		Addr:    "0.0.0.0:12345",
	}
	log.Printf("Webserver running")
	log.Fatal(srv.ListenAndServe())
}
