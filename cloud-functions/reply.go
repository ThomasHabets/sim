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
)

type Req struct {
	ID      string `json:"id"`
	Content []byte `json:"content"`
}

func bail(w http.ResponseWriter, s string, code int) {
	log.Print(s)
	w.WriteHeader(code)
	fmt.Fprint(w, s)
}

func MainHandler(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	app, err := firebase.NewApp(ctx, &firebase.Config{
		DatabaseURL: "https://simapprover.firebaseio.com",
	})
	if err != nil {
		bail(w, fmt.Sprintf("Error creating firebase app: %v", err), 500)
		return
	}

	data, err := ioutil.ReadAll(r.Body)
	if err != nil {
		bail(w, fmt.Sprintf("Failed to read request", err), 400)
		return
	}

	client, err := app.Database(ctx)
	if err != nil {
		bail(w, fmt.Sprintf("Error connecting to database: %v", err), 500)
		return
	}

	var req Req
	if err := json.Unmarshal(data, &req); err != nil {
		bail(w, fmt.Sprintf("Error parsing json: %v", err), 400)
		return
	}

	if err := client.NewRef("reply/"+req.ID).Set(ctx, req.Content); err != nil {
		bail(w, fmt.Sprintf("Failed to set value: %v", err), 500)
		return
	}
	// TODO: schedule a task to remove the entry.

	fmt.Println("Successfully stored")
	fmt.Fprint(w, "\nOK")
}
