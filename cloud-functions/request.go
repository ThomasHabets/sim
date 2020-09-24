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
package p

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"log"
	"net/http"

	firebase "firebase.google.com/go"
	"firebase.google.com/go/messaging"
)

type Req struct {
	Device  string `json:"device"`
	Content []byte `json:"content"`
}

func bail(w http.ResponseWriter, s string, code int) {
	log.Print(s)
	w.WriteHeader(code)
	fmt.Fprint(w, s)
}

func MainHandler(w http.ResponseWriter, r *http.Request) {
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

	message := &messaging.Message{
		Data: map[string]string{
			"request": string(req.Content),
		},
		Notification: &messaging.Notification{
			Title: "SimApprover",
			Body:  "New sim command to approve",
			// ImageURL: TODO
		},
		Token: req.Device,
	}

	response, err := client.Send(ctx, message)
	if err != nil {
		bail(w, fmt.Sprintf("Failed to send message: %v", err), 500)
		return
	}

	fmt.Println("Successfully sent message:", response)
	fmt.Fprint(w, "OK")
}
