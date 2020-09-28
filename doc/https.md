# Sim over HTTPS

Sim over HTTPS allows a remote user to approve commands. The only client that
currently exists is the [Android app][app].

Instead of running `approve` the user that is the approver has a constantly
running `approve-web` running.

The remote end needs to be able to connect to the server over HTTPS. Otherwise
approvals could be sniffed and spoofed.

To not require another certificate, and one where the approving user would have
to have access to the private key, the HTTPS has to be set up by the web server.

In the future there will be another layer of encryption added, so that the HTTPS
and webserver will not be needed.

## Approving your own commands out of band

If you want to approve your own commands you'll have to create a separate user
that `approve-web` runs as, that is not the same user as the one that runs
`sim`. This is because `sim` doesn't allow the same user to approve the
command, and working as intended.

## Setup

1. Select or create a user, member of the approver group, that will run
   `approve-web`
2. Build `approve-web`: `go build ./cmd/approve-web`
3. Run `approve-web` as the user: `SIM_PIN="some password here" ./approve-web`
4. Set up nginx to forward requests from anything starting with `/sim`:
   ```
   map $http_upgrade $connection_upgrade {
     default upgrade;
     '' close;
   }
   server {
     […]
     location /sim {
       proxy_read_timeout 300;
       proxy_pass  http://127.0.0.1:12345;
       proxy_http_version 1.1;
       proxy_set_header Upgrade $http_upgrade;
       proxy_set_header Connection $connection_upgrade;
     }
   }
   ```
5. Reload nginx config. E.g. `/etc/init.d/nginx reload`
6. Install [the app][app] on your phone
7. Press the settings button in top right
   1. `Enable websockets`
   1. Set the host
   1. Set the PIN to be the same password chosen above.


## If server is behind NAT, or otherwise can't run a webserver like this

The [cloud communication channel](cloud.md) is an alternative that takes
away the need for running your own webserver or needing to allow any
connections from the phone to the server.

[app]: https://play.google.com/store/apps/details?id=com.thomashabets.simapprover
