// @tags: [
//   requires_non_retryable_writes,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

// Sanity test for removing documents with adjacent index keys.  SERVER-2008

let t = db.jstests_removec;
t.drop();
t.createIndex({a: 1});

/** @return an array containing a sequence of numbers from i to i + 10. */
function runStartingWith(i) {
    let ret = [];
    for (let j = 0; j < 11; ++j) {
        ret.push(i + j);
    }
    return ret;
}

// Insert some documents with adjacent index keys.
for (let i = 0; i < 1100; i += 11) {
    t.save({a: runStartingWith(i)});
}

// Remove and then reinsert random documents in the background.
let s = startParallelShell('t = db.jstests_removec;' +
                           'Random.setRandomSeed();' +
                           'for( j = 0; j < 1000; ++j ) {' +
                           '    o = t.findOne( { a:Random.randInt( 1100 ) } );' +
                           '    t.remove( { _id:o._id } );' +
                           '    t.insert( o );' +
                           '}');

// Find operations are error free. Note that the cursor throws if it detects the $err
// field in the returned document.
for (let i = 0; i < 200; ++i) {
    t.find({a: {$gte: 0}}).hint({a: 1}).itcount();
}

s();

t.drop();
