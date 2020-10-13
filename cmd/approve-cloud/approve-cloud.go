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
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"net/http"
	"os"
	"path"
	"strings"
	"time"

	"github.com/fsnotify/fsnotify"
	"github.com/google/tink/go/aead/subtle"
	log "github.com/sirupsen/logrus"

	"github.com/ThomasHabets/sim/pkg/sim"
)

var (
	devices = flag.String("devices", "", "Registration tokens of phones, comma separated.")
	sockDir = flag.String("sock_dir", "/var/run/sim", "Socket directory.")
	cloud   = flag.String("cloud", "https://europe-west2-simapprover.cloudfunctions.net/", "Cloud base URL.")
)

func get_pin() (string, error) {
	ret := os.Getenv("SIM_PIN")
	if ret == "" {
		return "", fmt.Errorf("empty pin")
	}
	return ret, nil
}

func get_key() ([]byte, error) {
	h := sha256.New()
	pin, err := get_pin()
	if err != nil {
		return nil, err
	}
	h.Write([]byte(pin))
	sum := h.Sum(nil)
	return sum, nil
}

func encrypt_raw(in []byte) ([]byte, error) {
	key, err := get_key()
	if err != nil {
		return nil, err
	}
	aes, err := subtle.NewAESGCM(key)
	if err != nil {
		return nil, err
	}
	enc, err := aes.Encrypt(in, nil)
	if err != nil {
		return nil, err
	}
	return enc, nil
}

func encrypt(in []byte) (string, error) {
	enc, err := encrypt_raw(in)
	return base64.StdEncoding.EncodeToString(enc), err
}

func decrypt(in string) ([]byte, error) {
	key, err := get_key()
	if err != nil {
		return nil, err
	}
	aes, err := subtle.NewAESGCM(key)
	if err != nil {
		return nil, err
	}
	in2, err := base64.StdEncoding.DecodeString(in)
	if err != nil {
		return nil, err
	}
	return aes.Decrypt(in2, nil)
}

func init() {
	plain := "hello world"
	enc, err := encrypt([]byte(plain))
	if err != nil {
		log.Fatalf("Encryption test failed at encrypt: %v", err)
	}
	dec, err := decrypt(enc)
	if err != nil {
		log.Fatalf("Encryption test failed at decrypt: %v", err)
	}
	if got, want := string(dec), plain; got != want {
		log.Fatalf("Encryption test failed at compare")
	}
}

func send(ctx context.Context, b []byte) error {
	type Req struct {
		Devices []string `json:"devices"`
		Content []byte
	}
	enc, err := encrypt(b)
	if err != nil {
		return err
	}
	req := Req{
		Devices: strings.Split(*devices, ","),
		Content: []byte(enc),
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

func poll(ctx context.Context, id, replyID string) error {
	type PollReq struct {
		ID string `json:"id"`
	}
	req := PollReq{
		ID: replyID,
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

	plain, err := decrypt(string(dat))
	if err != nil {
		return err
	}
	return sim.Reply(path.Join(*sockDir, id), plain)
}
func poll_loop(ctx context.Context, id, replyID string) {
	// TODO: exponential backoff.
	ctx, cancel := context.WithTimeout(ctx, time.Minute)
	defer cancel()
	for {
		if ctx.Err() != nil {
			log.Infof("poll timeout")
			return
		}
		if err := poll(ctx, id, replyID); err != nil {
			log.Errorf("Poll error id %q reply-id %q: %v", id, replyID, err)
			time.Sleep(2 * time.Second)
			continue
		}
		log.Infof("Got reply for id %q reply-id %q", id, replyID)
		return
	}
}

func genReplyID(id string) (string, error) {
	key, err := get_key()
	if err != nil {
		return "", err
	}
	mac := hmac.New(sha256.New, key)
	mac.Write([]byte(id))
	return hex.EncodeToString(mac.Sum(nil)), nil
}

func main() {
	flag.Parse()

	if *devices == "" {
		log.Fatalf("Need a device token -device")
	}

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
				fn := path.Base(event.Name)
				replyID, err := genReplyID(fn)
				if err != nil {
					log.Errorf("Failed to create replyid: %v", err)
					break
				}

				// Send it.
				if err := send(ctx, req); err != nil {
					log.Errorf("Failed to send: %v", err)
					break
				}
				go poll_loop(ctx, fn, replyID)

			}
		}()
	}
}
