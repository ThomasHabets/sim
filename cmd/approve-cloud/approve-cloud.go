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
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"net/http"
	"path"
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

func send(ctx context.Context, b []byte) error {
	type Req struct {
		Device  string `json:"device"`
		Content []byte
	}
	req := Req{
		Device:  *device,
		Content: b,
	}
	reqbs, err := json.Marshal(&req)
	if err != nil {
		return err
	}

	r, err := http.NewRequestWithContext(ctx, "POST", *cloud+"request", bytes.NewBuffer(reqbs))
	if err != nil {
		return err
	}

	resp, err := http.DefaultClient.Do(r)
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("status code non-200: %d", resp.StatusCode)
	}
	log.Infof("Supposedly succussfully sent FCM message to %q", *cloud+"request")
	return nil
}

func poll(ctx context.Context, id string) error {
	type PollReq struct {
		ID string `json:"id"`
	}
	req := PollReq{
		ID: id,
	}
	reqb, err := json.Marshal(&req)
	if err != nil {
		return err
	}
	r, err := http.NewRequestWithContext(ctx, "POST", *cloud+"poll-reply", bytes.NewBuffer(reqb))
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(r)
	if err != nil {
		return err
	}
	dat, err := ioutil.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("status code non-200: %d", resp.StatusCode)
	}
	if len(dat) == 0 {
		return errors.New("not there")
	}
	return sim.Reply(path.Join(*sockDir, req.ID), dat)
}
func poll_loop(ctx context.Context, id string) {
	// TODO: exponential backoff.
	for {
		if err := poll(ctx, id); err != nil {
			log.Errorf("Poll error id %q: %v", id, err)
			time.Sleep(2 * time.Second)
			continue
		}
		log.Infof("Got reply for %q", id)
		return
	}
}
func main() {
	flag.Parse()
	watcher := sim.NewWatcher(*sockDir)
	go watcher.Run()
	ctx := context.Background()

	log.Infof("Running…")
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
				go poll_loop(ctx, path.Base(event.Name))

				// Send it.
				if err := send(ctx, req); err != nil {
					log.Errorf("Failed to send: %v", err)
				}
			}
		}()
	}
}
