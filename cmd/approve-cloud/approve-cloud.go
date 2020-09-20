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

import (
	"bytes"
	"context"
	"flag"
	"net/http"
	"time"

	"github.com/fsnotify/fsnotify"
	log "github.com/sirupsen/logrus"

	"github.com/ThomasHabets/sim/pkg/sim"
)

var (
	device  = flag.String("device", "", "Registration token of a phone.")
	sockDir = flag.String("sock_dir", "/var/run/sim", "Socket directory.")
	cloud   = flag.String("cloud", "https://europe-west2-simapprover.cloudfunctions.net/", "Cloud base URL.")
)

func send(ctx context.Context, req []byte) error {
	r, err := http.NewRequestWithContext(ctx, "POST", *cloud+"ask", bytes.NewBuffer(req))
	if err != nil {
		return err
	}

	resp, err := http.DefaultClient.Do(r)
	if err != nil {
		return err
	}
	_ = resp
	return nil
}

func main() {
	flag.Parse()
	watcher := sim.NewWatcher(*sockDir)
	go watcher.Run()
	ctx := context.Background()
	for {
		func() {
			cancel, ch := watcher.Add()
			defer cancel()

			select {
			case event, ok := <-ch:
				if event.Op != fsnotify.Create {
					break
				}
				time.Sleep(time.Second)
				log.Infof("Picked up a request with %q", event.Name)
				if !ok {
					return
				}

				// Read request.
				req, err := sim.ReadRequest(event.Name)
				if err != nil {
					log.Errorf("Failed to read request: %v", err)
					break
				}

				// Send it.
				if err := send(ctx, req); err != nil {
					log.Errorf("Failed to send: %v", err)
				}
			}
		}()
	}
}
