# gitm - the New way to send pathches over email 

gitm is a new utility to send git patches over email. Its a CLI Tool that builds `/gitm` patches and (sometimes, using system API), send it over to the Email of the recipient . 

## Typical gitm file structure - 

Gitm file struct is as follows - 
```
[4 bytes] Magi characters (GITM) 
[n bytes] gitm.manifest 

[n bytes] git commits object 
```

## Licensed under the GNU GPL v3.0