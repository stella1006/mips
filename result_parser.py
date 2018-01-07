#!/usr/bin/env python3

import csv
import ast
import os
import matplotlib.pyplot as plt
from collections import defaultdict, namedtuple

parameter_tuples = {
    "Flat": namedtuple("FlatParameters", []),
    "IVF": namedtuple("IVFParameters", ["nprobe", "size"]),
    "KMeans": namedtuple("KMeansParameters", ["U", "bnb",
        "layers", "m", "nprobe", "nprobe_test", "spherical"]),
    "Alsh": namedtuple("AlshParameters", ["K", "L", "U", "m", "r"]),
    "Quant": namedtuple("QuantParameters", [
        "centroid_count", "subspace_count"]),
}

# Colors hardcoded to improve contrast between KMeans and IVF, which are
# heavily overlapping.
colors = {
    "Flat": "c",
    "IVF": "g",
    "KMeans": "r",
    "KMeans-1": "r",
    "KMeans-2": "m",
    "KMeans-3": "b",
    "Alsh": "b",
    "Quant": "y",
}

def add(results, row):
    if row["status"] == "FAIL":
        return

    if row["timeout"] == "True":
        return

    index = row["cls_name"].replace("Index", "")
    data = row["data_path"].rstrip("/").split("/")[-1]
    key = (index, data)

    tup = parameter_tuples[key[0]]
    params = tup(**{k: row[k] for k in tup._fields})

    try:
        value = {
            "p100" : float(row["p_at_00100"]),
            "p25"  : float(row["p_at_00025"]),
            "p5"   : float(row["p_at_00005"]),
            "p1"   : float(row["p_at_00001"]),
            "train": float(row["train-time"]),
            "test" : float(row["test-time"]),
        }
    except ValueError:
        return
    results[key][params] = value


def parse_csv(name):
    s = open(name).read()
    s = s[s.find("\n") + 1:].splitlines()

    results = defaultdict(dict)

    reader = csv.DictReader(s)
    for row in reader:
        if row["cls_name"] == "KMeansIndex":
            tests = ast.literal_eval(row["nprobe_test"])
            for t in tests:
                row2 = row.copy()
                row2["nprobe_test"] = t
                for k in row2:
                    if k.endswith("-%05d" % t):
                        k2 = k[:k.rfind("-")]
                        row2[k2] = row2[k]
                add(results, row2)
        else:
            add(results, row)

    return results

def get_nondominated(rows, p):
    s = [(rows[k], k) for k in rows]
    s = sorted(s, key = lambda x: x[0]["test"])
    res = []
    for row in s:
        if not res or row[0][p] > res[-1][0][p]:
            res.append(row)
    return res
    

def get_all_nondominated(results):
    indexes = set(k[0] for k in results)
    datasets = set(k[1] for k in results)
    precisions = ["p1", "p5", "p25", "p100"]

    nondominated = {}
    for i in indexes:
        for d in datasets:
            for p in precisions:
                nondominated[(i, d, p)] = get_nondominated(
                        results[(i, d)], p) 

    return nondominated
    

def get_almost_nondominated(results, nondominated):
    almost_nondominated = {}
    for i, d, p in nondominated:
        nonrows = nondominated[(i, d, p)]
        r = results[(i, d)]
        rows = [(r[k], k) for k in r]
        # Select all rows which are not dominated, all dominated by
        # a small margin - up to 10% time difference and up to 10%
        # of precision deficit difference.
        max_dp = next(iter(results[("Flat", d)].values()))[p]
        good = []
        for row in rows:
            ok = False
            for nonrow in nonrows:
                dt = row[0]["test"] - nonrow[0]["test"]
                dpn = max(max_dp - nonrow[0][p], 0)
                dpr = max(max_dp - row[0][p], 0)
                dp = dpr - dpn
                dpn += 1e-18 # Avoid division by zero.
                if dt / nonrow[0]["test"] < 0.1 and dp / dpn < 0.1:
                    ok = True
                    break
            if ok:
                good.append(row)
        almost_nondominated[(i, d, p)] = sorted(good, key = lambda x: x[0]["test"])

    return almost_nondominated


def draw_nondominated(nondominated, name, step = True):
    indexes = sorted({k[0] for k in nondominated.keys()})
    datasets = sorted({k[1] for k in nondominated.keys()})
    precisions = sorted({k[2] for k in nondominated.keys()})

    for p in precisions:
        for d in datasets:
            print(d, p)
            graph_data = {i: nondominated[(i, d, p)] for i in indexes}
            baseline_time = next(iter(graph_data["Flat"]))[0]["test"]
            del graph_data["Flat"]
            
            plt.clf()
            lines = []
            for i in graph_data:
                if not graph_data[i]:
                    continue
                t = [k[0]["test"] / baseline_time for k in graph_data[i]]
                prec = [k[0][p] for k in graph_data[i]]
                if step:
                    lines.append(plt.step(t, prec, label = i, where = "post", color = colors[i])[0])
                else:
                    lines.append(plt.plot(t, prec, colors[i] + ".", label = i, marker = ".")[0])
            plt.legend(handles = lines)
            maxt = {"Amazon-3M": 0.1, "sift": 0.5, "siftsmall": 0.04, "WikiLSHTC": 0.2}
            maxt = maxt[d]
            plt.xlim(0, maxt)
            plt.ylim(ymin = 0)
            plt.ylabel("Precision")
            plt.xlabel("Fraction of brute force time")
            plt.title(d)
            plt.savefig("graphs/%s/%s-%s.eps" % (name, d, p))
            plt.savefig("graphs/%s/%s-%s.png" % (name, d, p))


def vary(results, dataset, p, index, params, what, values):
    data = results[(index, dataset)]
    plt.clf()
    arr = []
    xs = []
    print(next(iter(data)))
    for val in values:
        pr = params._replace(**{what: val})
        try:
            arr.append(data[pr]["test"])
            xs.append(float(val))
        except KeyError:
            pass
    print(xs, arr)
    plt.plot(xs, arr)
    plt.xscale("log")
    plt.title("%s query time on %s dataset" % (index, index))
    plt.ylabel("seconds")
    plt.xlabel(what)
    plt.savefig("graphs/vary/%s-%s-%s-%s.eps" % (dataset, index, p, what))
    plt.savefig("graphs/vary/%s-%s-%s-%s.png" % (dataset, index, p, what))


def split_kmeans(results):
    km = defaultdict(dict)
    for index, dataset in results:
        if index in ["Flat", "IVF"]:
            km[(index, dataset)] = results[(index, dataset)]
        if index != "KMeans": continue
        data = results[(index, dataset)]
        for k in data:
            km[("KMeans-%s" % k.layers, dataset)][k] = data[k]
    return km


def make_dirs():
    for d in ["nondominated", "almost-nondominated", "nondominated-km", "almost-nondominated-km", "vary"]:
        try:
            os.makedirs("graphs/" + d)
        except OSError:
            pass


def main():
    make_dirs()
    results = parse_csv("results/results-2018-01-02--19_50.csv")
    results_km = split_kmeans(results)
    nondominated = get_all_nondominated(results)
    nondominated_km = get_all_nondominated(results_km)
    almost = get_almost_nondominated(results, nondominated)
    almost_km = get_almost_nondominated(results_km, nondominated_km)
    draw_nondominated(nondominated, "nondominated")
    #draw_nondominated(almost, "almost-nondominated", False)
    draw_nondominated(nondominated_km, "nondominated-km")
    #draw_nondominated(almost_km, "almost-nondominated-km", False)

    indexes = sorted({k[0] for k in nondominated.keys()})
    datasets = sorted({k[1] for k in nondominated.keys()})
    precisions = sorted({k[2] for k in nondominated.keys()})

    vary(results, "sift", "p25", "KMeans", parameter_tuples["KMeans"](U = "0.85",
        layers = "3", m = "0", nprobe = "32", nprobe_test = 64, spherical = "False", bnb = "False"),
        "nprobe", [str(s) for s in [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096]])

    return
    for i in indexes:
        for d in datasets:
            print("\n\n%s, %s\n\n" % (i, d))
            for row, params in almost[(i, d, "p5")]:
                print(row["test"], row["p5"], params)


if __name__ == "__main__":
    main()
