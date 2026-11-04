import 'dart:convert';

/// The Life plan: which healthy habits the watch helps with, and when.
/// Sent to the watch as {"t":"life",...} (see firmware/app_life.h).
class LifePlan {
  // nudges only between these (minutes after midnight)
  final int dayFrom, dayTo;
  // water: glasses a day, a nudge every [waterEvery] minutes
  final bool water;
  final int waterGoal, waterEvery;
  // stretch breaks in working hours
  final bool stretch;
  final int stretchGoal, stretchEvery, workFrom, workTo;
  // training: days (bit 0 = Sunday), time, exercise minutes
  final bool train;
  final int trainDays, trainAt, trainGoal;
  // mindful minutes: a nudge at [mindAt]
  final bool mind;
  final int mindGoal, mindAt;
  // meals: times and the kind of food for the ideas
  final bool meals;
  final int breakfastAt, lunchAt, dinnerAt;
  final String diet; // omnivore, mediterranean, vegetarian, vegan, high_protein
  final bool aiMeals; // ideas written by the on-phone LLM (when one is installed)
  final String foodNotes; // allergies, likes, dislikes - for the LLM
  // social: someone to check in with on [socialDays] at [socialAt]
  final bool social;
  final int socialDays, socialAt;
  final String socialWho;
  // wind down before bed
  final bool wind;
  final int windAt;

  const LifePlan({
    this.dayFrom = 8 * 60,
    this.dayTo = 22 * 60,
    this.water = true,
    this.waterGoal = 8,
    this.waterEvery = 90,
    this.stretch = true,
    this.stretchGoal = 3,
    this.stretchEvery = 120,
    this.workFrom = 9 * 60,
    this.workTo = 18 * 60,
    this.train = false,
    this.trainDays = 0x2A, // Mon, Wed, Fri
    this.trainAt = 18 * 60,
    this.trainGoal = 30,
    this.mind = true,
    this.mindGoal = 3,
    this.mindAt = 21 * 60,
    this.meals = true,
    this.breakfastAt = 8 * 60,
    this.lunchAt = 13 * 60,
    this.dinnerAt = 19 * 60 + 30,
    this.diet = 'mediterranean',
    this.aiMeals = true,
    this.foodNotes = '',
    this.social = false,
    this.socialDays = 0x41, // Sun, Sat
    this.socialAt = 19 * 60,
    this.socialWho = '',
    this.wind = true,
    this.windAt = 22 * 60 + 30,
  });

  Map<String, dynamic> toJson() => {
    'df': dayFrom,
    'dt': dayTo,
    'w': {'on': water ? 1 : 0, 'g': waterGoal, 'ev': waterEvery},
    's': {'on': stretch ? 1 : 0, 'g': stretchGoal, 'ev': stretchEvery, 'f': workFrom, 't': workTo},
    'tr': {'on': train ? 1 : 0, 'dy': trainDays, 'g': trainGoal, 'at': trainAt},
    'm': {'on': mind ? 1 : 0, 'g': mindGoal, 'at': mindAt},
    'me': {'on': meals ? 1 : 0, 'b': breakfastAt, 'l': lunchAt, 'd': dinnerAt},
    'so': {'on': social ? 1 : 0, 'dy': socialDays, 'at': socialAt, 'who': socialWho},
    'wd': {'on': wind ? 1 : 0, 'at': windAt},
    'diet': diet,
    'ai': aiMeals ? 1 : 0,
    'notes': foodNotes,
  };

  /// The watch's message: the plan without the app-only bits.
  Map<String, dynamic> toWatch() => {
    't': 'life',
    ...toJson()
      ..remove('diet')
      ..remove('ai')
      ..remove('notes'),
  };

  static LifePlan fromJson(Map<String, dynamic> j) {
    int i(Map? m, String k, int d) => (m?[k] as num?)?.toInt() ?? d;
    bool b(Map? m, bool d) => m == null ? d : m['on'] == 1;
    const d = LifePlan();
    final w = j['w'] as Map?, s = j['s'] as Map?, t = j['tr'] as Map?, m = j['m'] as Map?;
    final me = j['me'] as Map?, so = j['so'] as Map?, wd = j['wd'] as Map?;
    return LifePlan(
      dayFrom: (j['df'] as num?)?.toInt() ?? d.dayFrom,
      dayTo: (j['dt'] as num?)?.toInt() ?? d.dayTo,
      water: b(w, d.water),
      waterGoal: i(w, 'g', d.waterGoal),
      waterEvery: i(w, 'ev', d.waterEvery),
      stretch: b(s, d.stretch),
      stretchGoal: i(s, 'g', d.stretchGoal),
      stretchEvery: i(s, 'ev', d.stretchEvery),
      workFrom: i(s, 'f', d.workFrom),
      workTo: i(s, 't', d.workTo),
      train: b(t, d.train),
      trainDays: i(t, 'dy', d.trainDays),
      trainGoal: i(t, 'g', d.trainGoal),
      trainAt: i(t, 'at', d.trainAt),
      mind: b(m, d.mind),
      mindGoal: i(m, 'g', d.mindGoal),
      mindAt: i(m, 'at', d.mindAt),
      meals: b(me, d.meals),
      breakfastAt: i(me, 'b', d.breakfastAt),
      lunchAt: i(me, 'l', d.lunchAt),
      dinnerAt: i(me, 'd', d.dinnerAt),
      diet: j['diet'] as String? ?? d.diet,
      aiMeals: j['ai'] == null ? d.aiMeals : j['ai'] == 1,
      foodNotes: j['notes'] as String? ?? '',
      social: b(so, d.social),
      socialDays: i(so, 'dy', d.socialDays),
      socialAt: i(so, 'at', d.socialAt),
      socialWho: so?['who'] as String? ?? '',
      wind: b(wd, d.wind),
      windAt: i(wd, 'at', d.windAt),
    );
  }

  /// A copy with some fields replaced (by their JSON path, e.g. ('w','g')).
  LifePlan edit(void Function(Map<String, dynamic> j) change) {
    final j = jsonDecode(jsonEncode(toJson())) as Map<String, dynamic>;
    change(j);
    return LifePlan.fromJson(j);
  }
}

/// One day as the watch counted it.
class LifeDay {
  final DateTime date;
  final int water, stretch, mind, train, meals, social, wind, score;
  const LifeDay(
    this.date,
    this.water,
    this.stretch,
    this.mind,
    this.train,
    this.meals,
    this.social,
    this.wind,
    this.score,
  );

  static DateTime _d(int ymd) => DateTime(ymd ~/ 10000, ymd ~/ 100 % 100, ymd % 100);

  static LifeDay? fromWatch(Map<String, dynamic> e) {
    final d = (e['d'] as num?)?.toInt() ?? 0;
    if (d < 20000101) return null;
    int g(String k) => (e[k] as num?)?.toInt() ?? 0;
    return LifeDay(_d(d), g('w'), g('st'), g('mi'), g('tr'), g('me'), g('so'), g('wd'), g('sc'));
  }

  static LifeDay? fromRow(List r) {
    if (r.length < 9) return null;
    final v = [for (final x in r) (x as num).toInt()];
    if (v[0] < 20000101) return null;
    return LifeDay(_d(v[0]), v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
  }

  List<int> toRow() => [
    date.year * 10000 + date.month * 100 + date.day,
    water,
    stretch,
    mind,
    train,
    meals,
    social,
    wind,
    score,
  ];
}

/// Meal ideas: simple, balanced, mostly whole foods - picked per day so they
/// change but don't repeat for a while. Kept short: the watch shows ~70 letters.
class MealIdeas {
  static const diets = {
    'mediterranean': 'Mediterranean',
    'omnivore': 'A bit of everything',
    'vegetarian': 'Vegetarian',
    'vegan': 'Vegan',
    'high_protein': 'High protein',
  };

  static const _ideas = <String, List<List<String>>>{
    // [breakfast, lunch, dinner] lists
    'mediterranean': [
      [
        'Greek yogurt, berries and walnuts',
        'Wholegrain toast, tomato, olive oil and oregano',
        'Oats with figs, honey and almonds',
        'Ricotta on rye with pear slices',
        'Two eggs with spinach and wholegrain bread',
        'Fruit salad with yogurt and pistachios',
      ],
      [
        'Chickpea salad with feta, cucumber and olive oil',
        'Lentil soup with wholegrain bread',
        'Tuna, white beans, red onion and parsley',
        'Farro with roasted vegetables and mozzarella',
        'Wholewheat pasta with tomato, basil and parmesan',
        'Hummus wrap with grilled vegetables',
      ],
      [
        'Grilled salmon, quinoa and green beans',
        'Baked cod with potatoes, olives and tomatoes',
        'Vegetable minestrone and a slice of cheese',
        'Chicken souvlaki with salad and tzatziki',
        'Aubergine and chickpea stew with rice',
        'Prawns, courgette and lemon with wholewheat pasta',
      ],
    ],
    'omnivore': [
      [
        'Porridge with banana and peanut butter',
        'Scrambled eggs, avocado and toast',
        'Yogurt, granola and an apple',
        'Wholegrain cereal with milk and berries',
        'Omelette with peppers and cheese',
        'Smoothie: spinach, banana, oats, milk',
      ],
      [
        'Chicken, rice and roasted vegetables',
        'Turkey and salad wholegrain sandwich',
        'Beef and vegetable stir-fry with noodles',
        'Jacket potato with tuna and sweetcorn',
        'Quinoa bowl with egg, avocado and beans',
        'Soup and a cheese and tomato sandwich',
      ],
      [
        'Salmon, sweet potato and broccoli',
        'Chicken curry with brown rice and spinach',
        'Lean beef chilli with beans and rice',
        'Turkey meatballs, tomato sauce and pasta',
        'Omelette, salad and wholegrain bread',
        'Fish tacos with cabbage slaw',
      ],
    ],
    'vegetarian': [
      [
        'Oats with apple, cinnamon and walnuts',
        'Eggs on toast with mushrooms',
        'Yogurt with kiwi and seeds',
        'Cottage cheese, berries and honey',
        'Avocado toast with a boiled egg',
        'Bircher muesli with grated apple',
      ],
      [
        'Halloumi, couscous and roasted peppers',
        'Lentil and spinach dal with rice',
        'Caprese salad with wholegrain bread',
        'Bean and cheese quesadilla with salsa',
        'Egg fried rice with peas and carrots',
        'Pea and mint soup with a cheese scone',
      ],
      [
        'Vegetable lasagne and a green salad',
        'Chickpea curry with brown rice',
        'Mushroom risotto with parmesan',
        'Black bean burgers with sweet potato wedges',
        'Shakshuka with crusty bread',
        'Tofu and vegetable stir-fry',
      ],
    ],
    'vegan': [
      [
        'Oats with soy milk, banana and chia',
        'Peanut butter toast with strawberries',
        'Tofu scramble with spinach and tomatoes',
        'Smoothie bowl: berries, oats and seeds',
        'Wholegrain toast with hummus and cucumber',
        'Overnight oats with mango and coconut',
      ],
      [
        'Buddha bowl: quinoa, chickpeas and tahini',
        'Lentil soup with rye bread',
        'Falafel wrap with salad and hummus',
        'Bean chilli with rice',
        'Tofu, rice noodles and vegetables',
        'Pasta with lentil bolognese',
      ],
      [
        'Chickpea and spinach curry with rice',
        'Tempeh stir-fry with broccoli and cashews',
        'Black bean tacos with avocado',
        'Vegetable paella with peas and peppers',
        'Lentil shepherd\'s pie with greens',
        'Tofu poke bowl with edamame',
      ],
    ],
    'high_protein': [
      [
        'Greek yogurt, whey, oats and berries',
        'Three-egg omelette with ham and spinach',
        'Cottage cheese, pineapple and almonds',
        'Protein porridge with peanut butter',
        'Smoked salmon and eggs on rye',
        'Skyr with granola and banana',
      ],
      [
        'Chicken breast, quinoa and green beans',
        'Tuna steak salad with eggs and potatoes',
        'Turkey chilli with kidney beans',
        'Lean beef, rice and broccoli',
        'Prawn and edamame noodle bowl',
        'Chicken wrap with hummus and salad',
      ],
      [
        'Salmon, lentils and spinach',
        'Steak, sweet potato and asparagus',
        'Chicken stir-fry with cashews and rice',
        'Cod with chickpeas and tomatoes',
        'Turkey burger with salad and wedges',
        'Tofu, tempeh and vegetable curry',
      ],
    ],
  };

  /// Today's three ideas for [diet]; [shuffle] moves to the next set.
  static (String, String, String) forDay(String diet, DateTime day, {int shuffle = 0}) {
    final l = _ideas[diet] ?? _ideas['mediterranean']!;
    final n = DateTime(day.year, day.month, day.day).difference(DateTime(2024)).inDays + shuffle;
    // breakfast, lunch and dinner step at different rates so the days mix
    return (l[0][n % l[0].length], l[1][(n * 5 + 1) % l[1].length], l[2][(n * 7 + 2) % l[2].length]);
  }
}
