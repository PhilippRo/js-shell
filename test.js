function with_many_statements(){
	log("first");
	log("secound");
	log("third");
	for(var i = 0; i< 4; i++) {
		log("fourth");
	}
};

log("some is printed");
with_many_statements();
log("may stop here");
for(var i = 0; i < 1000; i++){
    log("frooooooooooooooooooooooooooooooo");
}
log("last expression");
