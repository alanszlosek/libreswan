/testing/guestbin/swan-prep
rm /etc/ipsec.secrets
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add west-rsasigkey-east-rsasigkey
ipsec auto --status
echo "initdone"