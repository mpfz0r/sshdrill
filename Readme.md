
[![Build Status](https://travis-ci.org/mpfz0r/sshdrill.svg?branch=master)](https://travis-ci.org/mpfz0r/sshdrill)

# sshdrill
A ssh wrapper to automate tunnel creation over multiple jump hosts.


## Description

Imagine you are logged into a customer's server.
To get there, you had to *ssh* through one or more jump hosts, and now you need to set up a port forwarding to debug a service.  
Configuring the intermediate tunnels to get a port forwarded through all the jump hosts can be tedious.
*sshdrill* automates this task for you.

*sshdrill* runs as a wrapper around your interactive *ssh* session.  It will listen for the *ssh* escape sequence '~C' to break into a command prompt which
accepts *ssh's* forwarding syntax.
*sshdrill* converts the requested forwarding into a series of forwardings for each hop on the way, scans how many nested *ssh* sessions are running, and applies the forwardings accordingly.


##Example Session

```
me@workstation:~$ alias ssh=sshdrill
me@workstation:~$ ssh jumphost1
user@jumphost1:~$ ssh jumphost2
user@jumphost2:~$ ssh root@target
root@target~$ 
 << ENTER ~C >>
 
sshdrill> -L8080:172.16.5.5:80
Forwarding port through 3 ssh sessions.

ssh> -L8080:172.16.5.5:80
Forwarding port.

ssh> -L8080:127.0.0.1:8080
Forwarding port.

ssh> -L8080:127.0.0.1:8080
Forwarding port.

root@target~$ 

```

The requested listen port in the forwarding specification is used for the intermediate tunnels. (port 8080 in the example above.)

## Features
*sshdrill* can tunnel local, remote and dynamic port forwardings through any number of *ssh* sessions.

```
sshdrill> h
Commands:
      -L[bind_address:]port:host:hostport    Request local forward
      -R[bind_address:]port:host:hostport    Request remote forward
      -D[bind_address:]port                  Request dynamic forward   
```
 
## Installation
### OSX
```
brew tap mpfz0r/mpfz0r
brew install sshdrill
```
### Debian
```
# apt-get install devscripts
$ git clone https://github.com/mpfz0r/sshdrill
$ cd sshdrill
$ debuild -uc -us -b
# dpkg -i ../sshdrill_*.deb
```
### Other
```
make -f Makefile.<Operatingsystem> install
```

## Todo
 * IPv6 forwardings are not supported yet.
 * The escape character cannot be configured.
