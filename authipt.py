# Copyright (c) 2010, Andreas Bertheussen <andreas@elektronisk.org>
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.

# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

import os, sys, time, pwd, fcntl
import syslog, socket
import signal, subprocess
import setproctitle

# termination handler does not exit directly. wantdeath should be checked
# regularly through the program
def needdeath(arg1, arg2):
	global wantdeath
	wantdeath = True
	return

# updaterules() and updateset() invoke ipset and iptables
def updaterules(add, userip, username):
	# TODO: me
	return True

def updateset(add, userip):
	ipsetargs = "/usr/sbin/ipset -N authipt iphash".split(' ')
	if add == True:
		# make sure set exists before adding ip to it
		try:
			ipset = subprocess.Popen(ipsetargs, shell=False, stderr=subprocess.PIPE)
		except OSError, (errno, strerror):
			syslog.syslog(LOG_ERR, "could not call ipset for table creation: %s" % strerror)
			return False
		
		ipset.wait()
		ipsetargs[1] = "-A"
	else:
		ipsetargs[1] = "-D"
	
	ipsetargs[3] = userip
	try:
		ipset = subprocess.Popen(ipsetargs, shell=False)
	except OSError, (errno, strerror):
		syslog.syslog(LOG_ERR, "could not call ipset for IP insertion/removal: %s" % strerror)
		return False

	ipset.wait()
	return True

def do_death(active):
	global pidfile, pidfilename, userip, username
	pidfile.close()
	if os.path.isfile(pidfile.name):
		os.unlink(pidfile.name)
	if active == True:
		updaterules(False, userip, username)
		updateset(False, userip)
	sys.exit()

	return

def printfile(filename):
	try:
		f = open(filename, "r")
		for line in f:
			print line
		f.close()
	except IOError:
		pass	# ignore errors
	return
LOG_INFO = syslog.LOG_INFO
LOG_ERR = syslog.LOG_ERR
syslog.openlog("authipt", syslog.LOG_PID|syslog.LOG_NDELAY, syslog.LOG_DAEMON)

# we must be root (uid 0) to run (through sudo)
if  os.getuid() != 0:
	syslog.syslog(LOG_ERR, "user with uid %i was denied to run authipt (not root)" % os.getuid())
	print "You are not root, %s." % pwd.getpwuid(os.getuid()).pw_name
	sys.exit()

try:
	SSH_TTY = os.environ['SSH_TTY']
	SSH_CONNECTION = os.environ['SSH_CONNECTION']
except KeyError:
	syslog.syslog(LOG_ERR, "SSH_TTY or SSH_CONNECTION were not provided")
	sys.exit()

try:
	userid = int(os.environ['SUDO_UID'])
	username = os.environ['SUDO_USER']
except KeyError:
	syslog.syslog(LOG_ERR, "SUDO_UID or SUDO_USER were not provided")
	sys.exit()

userpwd = pwd.getpwuid(userid)
if userpwd.pw_name != username:
	syslog.syslog(LOG_ERR, "SUDO_UID and SUDO_USER do not refer to same user")
	sys.exit()

# check the shell
#if userpwd.pw_shell != "/bin/authipt":
	#syslog.syslog(LOG_ERR, "User %s does not have authipt as shell" % username)
	#sys.exit()

if SSH_CONNECTION.count(' ') != 3:
	syslog.syslog(LOG_ERR, "SSH_CONNECTION was malformed")
	sys.exit()

userip = SSH_CONNECTION.split(' ', 2)[0]	# first element is ip

# do a sanity check of the IP address size. NOTE: must be extended for IPv6
if (len(userip) > 16) or (len(userip) < 7):
	syslog.syslog(LOG_ERR, "IP address \"%s\" was too long or short", userip)
	sys.exit()

try:
	socket.inet_pton(socket.AF_INET, userip)
except socket.error:
	syslog.syslog(LOG_ERR, "IP address \"%s\" was invalid" % userip )
	sys.exit()

pidfilename = "/var/authipt/%s" % userip

signal.signal(signal.SIGTERM, needdeath)
signal.signal(signal.SIGINT, needdeath)
signal.signal(signal.SIGQUIT, needdeath)
signal.signal(signal.SIGALRM, needdeath)
signal.signal(signal.SIGPIPE, needdeath)
signal.signal(signal.SIGHUP, needdeath)
signal.signal(signal.SIGTSTP, needdeath)

tries = 0
retry = True
wantdeath = False
while retry:
	retry = False
	try:
		if os.path.isfile(pidfilename):
			pidfile = open(pidfilename, "rw+")
		else:
			pidfile = open(pidfilename, "wr+")
	except IOError, (errno, strerror):
		syslog.syslog(LOG_ERR, "unable to open pidfile %s" % pidfilename)
		do_death(0)
	
	pidfiledesc = pidfile.fileno()
	try:
		fcntl.flock(pidfiledesc, fcntl.LOCK_EX|fcntl.LOCK_NB)
	except IOError:
		syslog.syslog(LOG_ERR, "pidfile %s is locked, killing owner" % pidfilename)
		retry = True
		pidfile.seek(0)
		lines = pidfile.readlines()
		if len(lines) != 2:
			syslog.syslog(LOG_ERR, "pidfile %s was corrupt" % pidfilename)
			pidfile.close()
			retry = True
		userpid = int(lines[0])	# first line is pid, second is username
		os.kill(userpid, signal.SIGTERM)

	tries += 1
	if tries == 5:
		syslog.syslog(LOG_ERR, "gave up grabbing lock for %s" % pidfilename)
		print "Authentication is unavailable due to technical difficulties"
		# TODO: blurb message
		time.sleep(60)
		do_death(0)
	if wantdeath == True:
		do_death(0)
	time.sleep(0.5)

# we have the pidfile lock, and can register a new authentication

banfilename = "/etc/authipt/users/%s/banned" % username
if os.path.isfile(banfilename):
	print "Your account is banned from authentication."
	syslog.syslog(LOG_INFO, "user %s was rejected due to existing banfile")
	printfile(banfilename)
	time.sleep(60)	# give time to read the message
	do_death(0)

try:
	pidfile.seek(0)
	pidfile.writelines(["%i\n" % os.getpid(), username])
	pidfile.truncate()
	pidfile.flush()
except IOError, (errno, strerror):
	syslog.syslog(LOG_ERR, "could not write pidfile %s: %s" % (pidfilename, strerror))

if not updaterules(True, userip, username):
	do_death(0)

if not updateset(True, userip):
	updaterules(False, userip, username)
	do_death(0)

syslog.syslog(LOG_INFO, "user %s@%s authenticated." % (username, userip))
print "Hello %s - you are authenticated from host %s." % (username, userip)
printfile("/etc/authipt/motd")
setproctitle.setproctitle("authipt: %s@%s" % (username, userip))

while True:	# sleep until killed.
	if wantdeath == True:
		do_death(1)
	time.sleep(5)
