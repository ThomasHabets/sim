# Sim over cloud messaging

The neatest way to send a request to a phone or other device for approval is to
use cloud messaging. Used to be called Google Cloud Messaging, now Firebase
Cloud Messaging (FCM).

## Setup

TODO

## Architecture

This section describes how data flows, and is not necessary to know in order to
use this system.

It's easy to receive messages on the phone using FCM, but sending them is
harder. Sending in either direction for our purposes is a bit tricky.

The "server" here is the machine where 'sim' is run. The "phone" is where the
app runs.

Note that while the request and response goes via Google servers, TODO: before
this is in production the request and response must be encrypted and signed
between the server and the phone. Not implemented yet, but not hard to do since
we already have the concept of shared secret.

That will prevent even Google from seeing or approving anything of yours. But of
course if Google breaks or blocks FCM, then your `sim` commands will not be able
to be approved by the app. The local `approve` command will be unaffected, since
it only uses a unix socket.

### From server to phone

In order to send FCM messages you need GCP credentials. You're not allowed to
ship those credentials in the source code, and if you generate your own they'll
only have access to *your* project, and therefore you need to create the rest of
the FCM project settings.

So Sim opted to instead rely on a Cloud Function. This means that the server
only needs to send an HTTPS request, and a Cloud Function will take care of
turning that into a FCM message.

The source code to this Cloud Function is `cloud-functions/request.go`

### From phone to server

While the phone could send a FCM message for the server to pick up, that would
again mean that the server would need GCP credentials. This is not a dependency
we want.

So instead the phone replies by HTTPS request to a Cloud Function. It stores the
response in a FireStore database.

The server polls via Cloud Functions for when an approval is accepted or
rejected.

Requests time out in 10 minutes.

TODO: Or it will be this way. This part is not yet implemented.

### Communication diagram

![Architecture](architecture.png)
