/*
 *    Copyright 2020-2024 Google LLC
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
package p

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"log"
	"net/http"

	firebase "firebase.google.com/go/v4"
	messaging "firebase.google.com/go/v4/messaging"
)

type Req struct {
	Device  string   `json:"device"`
	Devices []string `json:"devices"`
	Content []byte   `json:"content"`
}

func bail(w http.ResponseWriter, s string, code int) {
	log.Print(s)
	w.WriteHeader(code)
	fmt.Fprint(w, s)
}

func MainHandler(w http.ResponseWriter, r *http.Request) {
	log.Printf("Got request from %s", r.Header.Get("X-Forwarded-For"))

	ctx := r.Context()
	app, err := firebase.NewApp(ctx, nil /*config *Config*/)
	if err != nil {
		bail(w, fmt.Sprintf("Failed to create firebase app: %v", err), 500)
		return
	}

	client, err := app.Messaging(ctx)
	if err != nil {
		bail(w, fmt.Sprintf("Failed to connect to messaging: %v", err), 500)
		return
	}

	data, err := ioutil.ReadAll(r.Body)
	if err != nil {
		bail(w, fmt.Sprintf("Failed to read request: %v", err), 400)
		return
	}

	var req Req
	if err := json.Unmarshal(data, &req); err != nil {
		bail(w, fmt.Sprintf("Failed to parse request json: %v", err), 400)
		return
	}

	// TODO: remove support for .Device before 1.0
	devs := req.Devices
	if len(devs) == 0 {
		devs = []string{req.Device}
	}

	{
		message := &messaging.MulticastMessage{
			Data: map[string]string{
				"request": string(req.Content),
			},
			Tokens: devs,
		}

		response, err := client.SendEachForMulticast(ctx, message)
		if err != nil {
			bail(w, fmt.Sprintf("Failed to send data message: %v", err), 500)
			return
		}
		for _, r := range response.Responses {
			if !r.Success {
				log.Printf("ERROR: failed to post data message: %v", r)
			}
		}
		if response.FailureCount == 0 {
			fmt.Println("Successfully sent data message:", response)
		}
	}
	{
		message := &messaging.MulticastMessage{
			Notification: &messaging.Notification{
				Title: "SimApprover",
				Body:  "New sim command to approve",
				// ImageURL: TODO
			},
			Tokens: devs,
		}

		response, err := client.SendEachForMulticast(ctx, message)
		if err != nil {
			bail(w, fmt.Sprintf("Failed to send message: %v", err), 500)
			return
		}
		for _, r := range response.Responses {
			if !r.Success {
				log.Printf("ERROR: failed to post message: %v", r)
			}
		}
		if response.FailureCount == 0 {
			fmt.Println("Successfully sent message:", response)
		}
	}
	fmt.Fprint(w, "OK")
}
