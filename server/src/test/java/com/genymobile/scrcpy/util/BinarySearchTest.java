package com.genymobile.scrcpy.util;

import org.junit.Assert;
import org.junit.Test;

public class BinarySearchTest {

    @Test
    public void testFindLastTrue() {
        Assert.assertEquals(4, BinarySearch.findLastTrue(5, 15, i -> false));
        Assert.assertEquals(5, BinarySearch.findLastTrue(5, 15, i -> i < 6));
        Assert.assertEquals(6, BinarySearch.findLastTrue(5, 15, i -> i < 7));
        Assert.assertEquals(7, BinarySearch.findLastTrue(5, 15, i -> i < 8));
        Assert.assertEquals(8, BinarySearch.findLastTrue(5, 15, i -> i < 9));
        Assert.assertEquals(9, BinarySearch.findLastTrue(5, 15, i -> i < 10));
        Assert.assertEquals(10, BinarySearch.findLastTrue(5, 15, i -> i < 11));
        Assert.assertEquals(11, BinarySearch.findLastTrue(5, 15, i -> i < 12));
        Assert.assertEquals(12, BinarySearch.findLastTrue(5, 15, i -> i < 13));
        Assert.assertEquals(13, BinarySearch.findLastTrue(5, 15, i -> i < 14));
        Assert.assertEquals(14, BinarySearch.findLastTrue(5, 15, i -> i < 15));
        Assert.assertEquals(15, BinarySearch.findLastTrue(5, 15, i -> true));
    }
}
